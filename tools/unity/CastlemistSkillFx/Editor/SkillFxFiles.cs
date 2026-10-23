// castlemist skill effects: the reader for gw2dat_cli skillfx-export's files (no UnityEngine, so
// tools/unity/Tests~ runs it under .NET).
//
//   skill_set.json                 {skill_namespace, effect_namespace, skills: [{id, label, folder}]}
//   skills/<id>-<slug>/skill.json  {id, label, codename, effects: [{model, folder, source, ei_name,
//                                   codename, count, error?}, ...], ...}
//   models/<id>/                   one effect model, laid out as a world export's model
// (src/skillfx/export.cpp writes them; docs/superpowers/specs/2026-10-22-skill-effects-design.md).

using System.Collections.Generic;
using System.Text;

namespace Castlemist.SkillFx
{
    public sealed class SkillSetFile
    {
        public string SkillNamespace, EffectNamespace;
        public List<SkillRef> Skills = new List<SkillRef>();
    }

    public sealed class SkillRef
    {
        public long Id;
        public string Label, Folder;
    }

    public sealed class SkillFile
    {
        public long Id;
        public string Label, Codename;
        public List<EffectRef> Effects = new List<EffectRef>();
    }

    /// One effect of a skill. Folder is "" when the model failed to export (Error says why) or there
    /// is none.
    public sealed class EffectRef
    {
        public long Model;
        public string Folder, Source, EiName, Codename, Error;
        public int Count;
    }

    public static class SkillFxFiles
    {
        public static SkillSetFile ReadSkillSet(string json)
        {
            Dictionary<string, object> d = Root(json, "skill_set.json");
            var s = new SkillSetFile { SkillNamespace = Str(d, "skill_namespace"), EffectNamespace = Str(d, "effect_namespace") };
            foreach (Dictionary<string, object> k in Objects(d, "skills", "skill_set.json"))
                s.Skills.Add(new SkillRef { Id = Long(k, "id"), Label = Str(k, "label"), Folder = Str(k, "folder") });
            return s;
        }

        public static SkillFile ReadSkill(string json)
        {
            Dictionary<string, object> d = Root(json, "skill.json");
            var f = new SkillFile { Id = Long(d, "id"), Label = Str(d, "label"), Codename = Str(d, "codename") };
            foreach (Dictionary<string, object> e in Objects(d, "effects", "skill.json"))
                f.Effects.Add(new EffectRef
                {
                    Model = Long(e, "model"),
                    Folder = Str(e, "folder"),
                    Source = Str(e, "source"),
                    EiName = Str(e, "ei_name"),
                    Codename = Str(e, "codename"),
                    Error = e.ContainsKey("error") ? Str(e, "error") : null,
                    Count = (int)Long(e, "count"),
                });
            return f;
        }

        /// As skillfx::slug: lowercase, each run of characters outside [a-z0-9] one '-', "skill" if empty.
        public static string Slug(string label)
        {
            var sb = new StringBuilder();
            bool dash = false;
            foreach (char c0 in label ?? "")
            {
                char c = c0 >= 'A' && c0 <= 'Z' ? (char)(c0 - 'A' + 'a') : c0;
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                {
                    if (dash && sb.Length > 0) sb.Append('-');
                    sb.Append(c);
                    dash = false;
                }
                else dash = true;
            }
            return sb.Length == 0 ? "skill" : sb.ToString();
        }

        static Dictionary<string, object> Root(string json, string what)
        {
            var d = MiniJson.Parse(json) as Dictionary<string, object>;
            if (d == null) throw new ManifestException(what + ": not a JSON object");
            return d;
        }

        static IEnumerable<Dictionary<string, object>> Objects(Dictionary<string, object> d, string key, string what)
        {
            object v;
            if (!d.TryGetValue(key, out v) || !(v is List<object> list)) throw new ManifestException(what + ": no " + key + " array");
            foreach (object o in list)
                if (o is Dictionary<string, object> x) yield return x;
        }

        static string Str(Dictionary<string, object> d, string k)
        {
            object v;
            return d.TryGetValue(k, out v) && v is string s ? s : "";
        }

        static long Long(Dictionary<string, object> d, string k)
        {
            object v;
            return d.TryGetValue(k, out v) && v is double x ? (long)x : 0;
        }
    }
}
