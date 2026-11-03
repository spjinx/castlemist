// castlemist: the map's GW2 waypoints, vistas and landmarks as teleport points in a VRChat world
// (2026-10-25). The world importer (Waypoints.cs) fills it from the public GW2 API: names[k] /
// points[k] in the API's order, kinds[k] "waypoint" | "vista" | "landmark" | "unlock". Teleport(k)
// moves the local player there; UdonSharp has no event parameters, so a GMMenu button calls the
// numbered event _TP<k> (or sets `selected` and calls TeleportSelected).

#if CASTLEMIST_VRC
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

namespace Castlemist
{
    [UdonBehaviourSyncMode(BehaviourSyncMode.None)]
    public class GW2Waypoints : UdonSharpBehaviour
    {
        public string[] names;
        public string[] kinds;
        public Transform[] points;
        public int selected;
        public AudioSource arriveSound;   // 2026-10-27: GW2's waypoint whoosh (599049), heard by whoever teleports

        public void Teleport(int k)
        {
            if (points == null || k < 0 || k >= points.Length || points[k] == null) return;
            VRCPlayerApi me = Networking.LocalPlayer;
            if (me != null) me.TeleportTo(points[k].position, points[k].rotation);
            if (arriveSound != null) arriveSound.Play();
        }

        public void TeleportSelected() { Teleport(selected); }

        public void _TP0() { Teleport(0); }
        public void _TP1() { Teleport(1); }
        public void _TP2() { Teleport(2); }
        public void _TP3() { Teleport(3); }
        public void _TP4() { Teleport(4); }
        public void _TP5() { Teleport(5); }
        public void _TP6() { Teleport(6); }
        public void _TP7() { Teleport(7); }
        public void _TP8() { Teleport(8); }
        public void _TP9() { Teleport(9); }
        public void _TP10() { Teleport(10); }
        public void _TP11() { Teleport(11); }
        public void _TP12() { Teleport(12); }
        public void _TP13() { Teleport(13); }
        public void _TP14() { Teleport(14); }
        public void _TP15() { Teleport(15); }
        public void _TP16() { Teleport(16); }
        public void _TP17() { Teleport(17); }
        public void _TP18() { Teleport(18); }
        public void _TP19() { Teleport(19); }
        public void _TP20() { Teleport(20); }
        public void _TP21() { Teleport(21); }
        public void _TP22() { Teleport(22); }
        public void _TP23() { Teleport(23); }
        public void _TP24() { Teleport(24); }
        public void _TP25() { Teleport(25); }
        public void _TP26() { Teleport(26); }
        public void _TP27() { Teleport(27); }
        public void _TP28() { Teleport(28); }
        public void _TP29() { Teleport(29); }
        public void _TP30() { Teleport(30); }
        public void _TP31() { Teleport(31); }
        public void _TP32() { Teleport(32); }
        public void _TP33() { Teleport(33); }
        public void _TP34() { Teleport(34); }
        public void _TP35() { Teleport(35); }
        public void _TP36() { Teleport(36); }
        public void _TP37() { Teleport(37); }
        public void _TP38() { Teleport(38); }
        public void _TP39() { Teleport(39); }
        public void _TP40() { Teleport(40); }
        public void _TP41() { Teleport(41); }
        public void _TP42() { Teleport(42); }
        public void _TP43() { Teleport(43); }
        public void _TP44() { Teleport(44); }
        public void _TP45() { Teleport(45); }
        public void _TP46() { Teleport(46); }
        public void _TP47() { Teleport(47); }
        public void _TP48() { Teleport(48); }
        public void _TP49() { Teleport(49); }
        public void _TP50() { Teleport(50); }
        public void _TP51() { Teleport(51); }
        public void _TP52() { Teleport(52); }
        public void _TP53() { Teleport(53); }
        public void _TP54() { Teleport(54); }
        public void _TP55() { Teleport(55); }
        public void _TP56() { Teleport(56); }
        public void _TP57() { Teleport(57); }
        public void _TP58() { Teleport(58); }
        public void _TP59() { Teleport(59); }
        public void _TP60() { Teleport(60); }
        public void _TP61() { Teleport(61); }
        public void _TP62() { Teleport(62); }
        public void _TP63() { Teleport(63); }
    }
}
#endif
