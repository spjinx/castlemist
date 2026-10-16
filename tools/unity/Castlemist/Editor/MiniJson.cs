// castlemist world export: a minimal JSON reader for the importer.
//
// Spec docs/superpowers/specs/2026-10-15-world-export-design.md, D6 step 1.
// Unity's JsonUtility cannot read the manifest's nested arrays (matrices,
// rects), so the importer bundles this reader. It turns a JSON text into
//   object -> Dictionary<string, object>   array  -> List<object>
//   number -> double   string -> string   true/false -> bool   null -> null
// and throws FormatException (with the character offset) on anything else,
// including NaN/Infinity, trailing text and unterminated strings.
// No UnityEngine dependency, so it compiles outside Unity (tools/unity/Tests~).

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Castlemist
{
    public static class MiniJson
    {
        public static object Parse(string text)
        {
            if (text == null) throw new ArgumentNullException(nameof(text));
            var p = new Parser(text);
            p.SkipWs();
            object v = p.Value(0);
            p.SkipWs();
            if (!p.End) throw p.Error("trailing text");
            return v;
        }

        sealed class Parser
        {
            const int MaxDepth = 256;
            readonly string s;
            int i;

            public Parser(string text)
            {
                s = text;
                // A UTF-8 BOM read as text.
                if (s.Length > 0 && s[0] == '﻿') i = 1;
            }

            public bool End => i >= s.Length;

            public FormatException Error(string what)
            {
                return new FormatException("JSON: " + what + " at offset " + i);
            }

            public void SkipWs()
            {
                while (i < s.Length)
                {
                    char c = s[i];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
                    else break;
                }
            }

            public object Value(int depth)
            {
                if (depth > MaxDepth) throw Error("nesting too deep");
                if (End) throw Error("unexpected end");
                char c = s[i];
                switch (c)
                {
                    case '{': return Obj(depth);
                    case '[': return Arr(depth);
                    case '"': return Str();
                    case 't': Word("true"); return true;
                    case 'f': Word("false"); return false;
                    case 'n': Word("null"); return null;
                    default:
                        if (c == '-' || (c >= '0' && c <= '9')) return Num();
                        throw Error("unexpected '" + c + "'");
                }
            }

            void Word(string w)
            {
                if (string.CompareOrdinal(s, i, w, 0, w.Length) != 0) throw Error("expected " + w);
                i += w.Length;
            }

            Dictionary<string, object> Obj(int depth)
            {
                var d = new Dictionary<string, object>(StringComparer.Ordinal);
                ++i;
                SkipWs();
                if (!End && s[i] == '}') { ++i; return d; }
                while (true)
                {
                    SkipWs();
                    if (End || s[i] != '"') throw Error("expected a key");
                    string k = Str();
                    SkipWs();
                    if (End || s[i] != ':') throw Error("expected ':'");
                    ++i;
                    SkipWs();
                    d[k] = Value(depth + 1);
                    SkipWs();
                    if (End) throw Error("unterminated object");
                    if (s[i] == ',') { ++i; continue; }
                    if (s[i] == '}') { ++i; return d; }
                    throw Error("expected ',' or '}'");
                }
            }

            List<object> Arr(int depth)
            {
                var a = new List<object>();
                ++i;
                SkipWs();
                if (!End && s[i] == ']') { ++i; return a; }
                while (true)
                {
                    SkipWs();
                    a.Add(Value(depth + 1));
                    SkipWs();
                    if (End) throw Error("unterminated array");
                    if (s[i] == ',') { ++i; continue; }
                    if (s[i] == ']') { ++i; return a; }
                    throw Error("expected ',' or ']'");
                }
            }

            string Str()
            {
                ++i;  // opening quote
                var b = new StringBuilder();
                while (true)
                {
                    if (End) throw Error("unterminated string");
                    char c = s[i++];
                    if (c == '"') return b.ToString();
                    if (c < 0x20) throw Error("control character in string");
                    if (c != '\\') { b.Append(c); continue; }
                    if (End) throw Error("unterminated escape");
                    char e = s[i++];
                    switch (e)
                    {
                        case '"': b.Append('"'); break;
                        case '\\': b.Append('\\'); break;
                        case '/': b.Append('/'); break;
                        case 'b': b.Append('\b'); break;
                        case 'f': b.Append('\f'); break;
                        case 'n': b.Append('\n'); break;
                        case 'r': b.Append('\r'); break;
                        case 't': b.Append('\t'); break;
                        case 'u':
                            if (i + 4 > s.Length) throw Error("short \\u escape");
                            int code;
                            if (!int.TryParse(s.Substring(i, 4), NumberStyles.AllowHexSpecifier,
                                              CultureInfo.InvariantCulture, out code))
                                throw Error("bad \\u escape");
                            b.Append((char)code);  // surrogate pairs pass through as two chars
                            i += 4;
                            break;
                        default: throw Error("bad escape '\\" + e + "'");
                    }
                }
            }

            double Num()
            {
                int start = i;
                if (s[i] == '-') ++i;
                if (End) throw Error("bad number");
                if (s[i] == '0') ++i;
                else if (s[i] >= '1' && s[i] <= '9') Digits();
                else throw Error("bad number");
                if (!End && s[i] == '.') { ++i; if (!Digits()) throw Error("bad fraction"); }
                if (!End && (s[i] == 'e' || s[i] == 'E'))
                {
                    ++i;
                    if (!End && (s[i] == '+' || s[i] == '-')) ++i;
                    if (!Digits()) throw Error("bad exponent");
                }
                double v = double.Parse(s.Substring(start, i - start), NumberStyles.Float, CultureInfo.InvariantCulture);
                if (double.IsInfinity(v) || double.IsNaN(v)) throw Error("number out of range");
                return v;
            }

            bool Digits()
            {
                int start = i;
                while (!End && s[i] >= '0' && s[i] <= '9') ++i;
                return i > start;
            }
        }
    }
}
