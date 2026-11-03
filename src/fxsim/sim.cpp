/// @file
/// @brief The reference particle sim (see sim.h). Spawn rules: GrCloud.cpp as read
///        (docs/research/gw2-particle-system.md, 2026-07-15 .. 2026-10-28).

#include "castlemist/fxsim/sim.h"

#include <algorithm>
#include <cmath>

namespace castlemist::fxsim {

namespace {

fx::Range rng2(const float p[2]) { return fx::Range::from_min_max(p[0], p[1]); }

// Spawn offset by file spawnShape (table 0x141ee8e00, 2026-10-22 amendment). The shapes are
// read; the exact distributions inside them are not (approximation).
fx::Vec3 shape_offset(const ParticleEmitterCPU& e, fx::Rng& r) {
    const float lo = e.spawnRadius[0], hi = e.spawnRadius[1];
    constexpr float kTwoPi = 6.2831853f;
    switch (e.spawnShape) {
        case 0: {   // disc, radius min..max
            const float a = r.u01() * kTwoPi, rad = fx::sample(rng2(e.spawnRadius), r);
            return {std::cos(a) * rad, std::sin(a) * rad, 0};
        }
        case 1:     // line along X: max long, min thick
            return {(r.u01() * 2 - 1) * hi, (r.u01() * 2 - 1) * lo, 0};
        case 3:     // flat rectangle, half-sizes max x min
            return {(r.u01() * 2 - 1) * hi, (r.u01() * 2 - 1) * lo, 0};
        case 4: {   // sphere, radius min..max
            const float z = r.u01() * 2 - 1, a = r.u01() * kTwoPi, s = std::sqrt(std::max(0.0f, 1 - z * z));
            const float rad = fx::sample(rng2(e.spawnRadius), r);
            return {std::cos(a) * s * rad, std::sin(a) * s * rad, z * rad};
        }
        case 5: {   // cylinder: a ring at min, up to max high
            const float a = r.u01() * kTwoPi;
            return {std::cos(a) * lo, std::sin(a) * lo, r.u01() * hi};
        }
        default:    // 2 / 6 mesh surface and mesh: at the emitter (approximation)
            return {};
    }
}

fx::Vec3 xform_point(const float m[12], fx::Vec3 p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}
fx::Vec3 xform_dir(const float m[12], fx::Vec3 d) {
    return {m[0] * d.x + m[1] * d.y + m[2] * d.z, m[4] * d.x + m[5] * d.y + m[6] * d.z,
            m[8] * d.x + m[9] * d.y + m[10] * d.z};
}

}  // namespace

Sim::Sim(SimInput in, uint32_t seed, bool collectStats) : in_(std::move(in)), collect_(collectStats) {
    for (size_t i = 0; i < in_.emitters.size(); ++i)
        rng_.push_back(fx::Rng{seed ^ static_cast<uint32_t>(i * 0x9e3779b9u)});
    acc_.assign(in_.emitters.size(), 0.0f);
    burstFired_.assign(in_.emitters.size(), false);
    acc2_.resize(in_.emitters.size());
    stats_.emitters.resize(in_.emitters.size());
}

const Stats& Sim::stats() const {
    stats_.dropped = dropped_;
    for (size_t i = 0; i < acc2_.size(); ++i) {
        const Acc& a = acc2_[i];
        EmitterStats& e = stats_.emitters[i];
        std::vector<float> d = a.dist;
        if (!d.empty()) {
            std::sort(d.begin(), d.end());
            const float q[3] = {0.05f, 0.5f, 0.95f};
            for (int k = 0; k < 3; ++k) e.spread[k] = d[static_cast<size_t>(q[k] * static_cast<float>(d.size() - 1))];
        }
        e.meanSpeed = a.speedN ? static_cast<float>(a.speedSum / static_cast<double>(a.speedN)) : 0.0f;
        for (int k = 0; k < 5; ++k) {
            e.alphaAt[k] = a.n[k] ? static_cast<float>(a.alpha[k] / static_cast<double>(a.n[k])) : 0.0f;
            e.sizeAt[k] = a.n[k] ? static_cast<float>(a.size[k] / static_cast<double>(a.n[k])) : 0.0f;
        }
    }
    return stats_;
}

std::vector<std::string> Sim::approximations() const {
    return {"per-emitter seeds (the client's seeding is not read)",
            "wind and the noise field around each step (FUN_140acb1d0 reads them from the environment)",
            "spawn-shape distributions inside each shape; mesh shapes spawn at the emitter",
            "emitter transform applied to spawn position and velocity unless file flags 0x4",
            "flipbook without fps spread over the life (2026-07 reading)",
            "bones are visible from the start and never toggle (no clip played): bursts fire once"};
}

void Sim::spawn(size_t ei) {
    const EmitterIn& in = in_.emitters[ei];
    const ParticleEmitterCPU& e = in.e;
    fx::Rng& r = rng_[ei];
    const ParticleCloudCPU* c = in.cloud >= 0 && static_cast<size_t>(in.cloud) < in_.clouds.size()
                                    ? &in_.clouds[static_cast<size_t>(in.cloud)] : nullptr;
    fx::Vec3 off = shape_offset(e, r);
    const fx::Range dir[3] = {rng2(e.velocity[0]), rng2(e.velocity[1]), rng2(e.velocity[2])};
    float u[4];
    for (float& x : u) x = r.u01();
    fx::Vec3 vel = fx::spawn_velocity(dir, rng2(e.velocity[3]), u);
    const fx::Range adir[3] = {rng2(e.acceleration[0]), rng2(e.acceleration[1]), rng2(e.acceleration[2])};
    for (float& x : u) x = r.u01();
    fx::Vec3 acc = fx::spawn_velocity(adir, rng2(e.acceleration[3]), u);
    if (e.hasTransform && !(e.flags & 0x4)) {
        off = xform_point(e.transform, off);
        vel = xform_dir(e.transform, vel);
        acc = xform_dir(e.transform, acc);
    }
    if (c) {
        vel.x += c->velocity[0]; vel.y += c->velocity[1]; vel.z += c->velocity[2];
        acc.x += c->acceleration[0]; acc.y += c->acceleration[1]; acc.z += c->acceleration[2];
    }
    Particle p;
    p.emitter = static_cast<uint16_t>(ei);
    p.pos = {in.origin.x + off.x, in.origin.y + off.y, in.origin.z + off.z};
    p.vel = vel;
    p.accel = acc;
    p.life = std::max(1e-4f, fx::sample(rng2(e.lifetime), r));
    for (int k = 0; k < 4; ++k) {
        p.begin[k] = fx::sample(fx::Range::from_min_max(e.colorBegin[0][k], e.colorBegin[1][k]), r);
        p.end[k] = fx::sample(fx::Range::from_min_max(e.colorEnd[0][k], e.colorEnd[1][k]), r);
    }
    for (int k = 0; k < 2; ++k) {
        p.size0[k] = fx::sample(rng2(e.scaleInitial[k]), r);
        p.growth[k] = fx::sample(rng2(e.scaleChange[k]), r);
    }
    // Y = X under file flag 0x10 (runtime 0x800, 2026-10-22 amendment).
    if (e.flags & 0x10) { p.size0[1] = p.size0[0]; p.growth[1] = p.growth[0]; }
    constexpr float kDeg = 3.1415927f / 180.0f;   // the loader converts rotations from degrees
    p.rotation0 = fx::sample(rng2(e.rotationInitial), r) * kDeg;
    p.spin = fx::sample(rng2(e.rotationChange), r) * kDeg;
    p.rotation = p.rotation0;
    if ((e.flags & 0x1000) && r.u01() < 0.5f) p.spin = -p.spin;   // file 0x1000: random spin sign
    appearance(p);
    frame_.particles.push_back(p);
}

void Sim::appearance(Particle& p) const {
    const ParticleEmitterCPU& e = in_.emitters[p.emitter].e;
    const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
    const float w = fx::colour_weight(t, e.colorPeriod, rng2(e.colorFalloff));
    for (int k = 0; k < 4; ++k) p.colour[k] = p.begin[k] * (1 - w) + p.end[k] * w;
    p.colour[3] *= !e.opacityCurve.empty() ? fx::curve_eval(e.opacityCurve, t, 1.0f)
                                           : fx::opacity_preset_value(e.opacityCurvePreset, t, e.flags);
    const float sc = !e.scaleCurve.empty() ? fx::curve_eval(e.scaleCurve, t, 1.0f)
                                           : fx::scale_preset_value(e.scaleCurvePreset, t, e.flags);
    for (int k = 0; k < 2; ++k) p.size[k] = std::max(0.0f, (p.size0[k] + p.growth[k] * p.age) * sc);
    const float* rc = e.texCoordRect;
    const bool rect = rc[2] > rc[0] && rc[3] > rc[1];
    const float u0 = rect ? rc[0] : 0, v0 = rect ? rc[1] : 0, u1 = rect ? rc[2] : 1, v1 = rect ? rc[3] : 1;
    const auto& fb = e.flipbook;
    if (!fb.present || fb.columns == 0 || fb.rows == 0 || fb.count == 0) {
        p.frame = 0;
        p.uv[0] = u0; p.uv[1] = v0; p.uv[2] = u1; p.uv[3] = v1;
        return;
    }
    // fps > 0: frames at fps, looping; else the frames spread over the life (2026-07 reading).
    const int n = fb.count;
    const int f = fb.fps > 0 ? static_cast<int>(std::floor(p.age * fb.fps)) % n
                             : std::min(n - 1, static_cast<int>(t * n));
    p.frame = (fb.start + f) % (fb.columns * fb.rows);
    const int col = p.frame % fb.columns, row = p.frame / fb.columns;
    const float cw = (u1 - u0) / fb.columns, ch = (v1 - v0) / fb.rows;
    p.uv[0] = u0 + col * cw; p.uv[1] = v0 + row * ch;
    p.uv[2] = p.uv[0] + cw;  p.uv[3] = p.uv[1] + ch;
}

void Sim::spawn_group(size_t ei) {
    const ParticleEmitterCPU& e = in_.emitters[ei].e;
    const double want = std::max(0.0, std::round(static_cast<double>(fx::sample(rng2(e.spawnGroupSize), rng_[ei]))));
    const double room = static_cast<double>(kMaxParticles - std::min(kMaxParticles, frame_.particles.size()));
    const double n = std::min(want, room);
    dropped_ += static_cast<size_t>(want - n);   // past the cap: counted, never looped over
    for (long k = 0; k < static_cast<long>(n); ++k) {
        spawn(ei);
        ++stats_.emitters[ei].spawned;
    }
}

void Sim::step(float dt) {
    if (!(dt > 0) || !std::isfinite(dt)) return;
    frame_.time += dt;
    // age, move, expire
    auto& ps = frame_.particles;
    for (Particle& p : ps) {
        p.age += dt;
        const EmitterIn& in = in_.emitters[p.emitter];
        const ParticleCloudCPU* c = in.cloud >= 0 && static_cast<size_t>(in.cloud) < in_.clouds.size()
                                        ? &in_.clouds[static_cast<size_t>(in.cloud)] : nullptr;
        fx::integrate(p.pos, p.vel, p.accel, in.e.drag + (c ? c->drag : 0.0f), dt);
        // spin decays by rotationDrag and stops rather than reversing (FUN_140acb950)
        if (in.e.rotationDrag != 0) {
            const float s2 = (1.0f - in.e.rotationDrag * dt) * p.spin;
            p.spin = p.spin * s2 >= 0 ? s2 : 0.0f;
        }
        p.rotation += p.spin * dt;
        appearance(p);
    }
    for (const Particle& p : ps)
        if (p.age >= p.life) ++stats_.emitters[p.emitter].expired;
    ps.erase(std::remove_if(ps.begin(), ps.end(), [](const Particle& p) { return p.age >= p.life; }), ps.end());
    // spawn: accumulator per emitter, a group each period that passes the chance (percent)
    for (size_t ei = 0; ei < in_.emitters.size(); ++ei) {
        const ParticleEmitterCPU& e = in_.emitters[ei].e;
        if (e.emitterFlags & 0x8) {   // burst: one group when the bone turns visible (FUN_140acc370)
            if (burstFired_[ei]) continue;
            burstFired_[ei] = true;
            spawn_group(ei);
            continue;
        }
        if (!(e.spawnPeriod > 0) || !(e.spawnProbability > 0)) continue;
        acc_[ei] += dt;
        // Whole periods in one go: a period under the accumulator's precision would never
        // decrease it one subtraction at a time. Groups past kMaxGroupsPerStep are not run.
        const double due = std::floor(static_cast<double>(acc_[ei]) / e.spawnPeriod);
        if (!(due >= 1)) continue;
        acc_[ei] = std::fmod(acc_[ei], e.spawnPeriod);
        const long groups = static_cast<long>(std::min(due, static_cast<double>(kMaxGroupsPerStep)));
        for (long g = 0; g < groups; ++g) {
            if (rng_[ei].u01() > e.spawnProbability * 0.01f) continue;
            spawn_group(ei);
        }
    }
    // per-step samples (none without collect_: the app keeps no history)
    if (!collect_) return;
    constexpr size_t kDistCap = 262144;
    std::vector<uint32_t> live(in_.emitters.size(), 0);
    for (const Particle& p : ps) {
        ++live[p.emitter];
        Acc& a = acc2_[p.emitter];
        const fx::Vec3 o = in_.emitters[p.emitter].origin;
        const float dx = p.pos.x - o.x, dy = p.pos.y - o.y, dz = p.pos.z - o.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(d)) {
            // reservoir sample over the whole run (its own RNG: the particles' sequences stay put)
            ++a.seen;
            if (a.dist.size() < kDistCap) a.dist.push_back(d);
            else if (const size_t j = static_cast<size_t>(statsRng_.u01() * static_cast<float>(a.seen)); j < kDistCap)
                a.dist[j] = d;
        }
        const float sp = std::sqrt(p.vel.x * p.vel.x + p.vel.y * p.vel.y + p.vel.z * p.vel.z);
        if (std::isfinite(sp)) { a.speedSum += sp; ++a.speedN; }
        const float t = p.age / p.life;
        const int bin = static_cast<int>(std::lround(t * 4.0f));
        if (bin >= 0 && bin <= 4 && std::fabs(t * 4.0f - static_cast<float>(bin)) <= 0.5f &&
            std::isfinite(p.colour[3]) && std::isfinite(p.size[0])) {
            a.alpha[bin] += p.colour[3];
            a.size[bin] += p.size[0];
            ++a.n[bin];
        }
    }
    for (size_t i = 0; i < live.size(); ++i) stats_.emitters[i].live.push_back(live[i]);
    stats_.live.push_back(static_cast<uint32_t>(ps.size()));
}

void seek(Sim& sim, const SimInput& in, uint32_t seed, float dt, float t, bool collectStats) {
    sim = Sim(in, seed, collectStats);
    if (!(dt > 0) || !(t > 0)) return;
    const long steps = std::lround(t / dt);
    for (long i = 0; i < steps; ++i) sim.step(dt);
}

}  // namespace castlemist::fxsim
