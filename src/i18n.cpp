/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the GUI's languages: see i18n.h. The language files are gettext's .po, read here (no libintl).
 *  The matching line by line and around the colour spans after the CX8ANM edition's i18n.cpp (condoruy).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "i18n.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <ctype.h>
#include <deque>
#include <dirent.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#ifndef I18N_NO_BOARD   /* the host test gives the directories itself */
#include "board.h"
#endif

#ifndef I18N_DIR
#define I18N_DIR      BOARD_SHARE "/i18n"   /* the image's language files */
#endif
#ifndef I18N_USER_DIR
#define I18N_USER_DIR BOARD_DATA "/i18n"    /* a translator's copies, read first */
#endif
#ifndef I18N_FONT_DIR
#define I18N_FONT_DIR BOARD_SHARE "/fonts"  /* a font a language names, when it is not beside its file */
#endif

namespace {

struct lang_t {
    std::string code, name, status, font;
    std::deque<std::string> store;      /* the texts, at addresses that stay */
    std::unordered_map<std::string_view, const char *> dict;
    std::mutex perm_mux;                /* i18n_tr_static's texts put together line by line, kept */
    std::unordered_map<std::string, std::string> perm;
};

std::atomic<lang_t *> cur{nullptr};
/* A language loaded once is never freed: another thread may be reading it as the language changes (rarely) */
std::mutex           loaded_mux;
std::vector<lang_t *> loaded;

/* ---- the .po file ---- */

struct po_entry {
    std::string ctx, id, str;
    bool        has_ctx = false, fuzzy = false, plural = false;
};

/* A quoted string of a .po line, C escapes undone, appended */
bool unquote(const char *p, std::string &out) {
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return false;
    for (p++; *p && *p != '"'; p++) {
        if (*p != '\\' || !p[1]) {
            out += *p;
            continue;
        }
        switch (*++p) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default: out += '\\'; out += *p; break;
        }
    }
    return *p == '"';
}

/* Each entry of the text to take(); take() false stops (the header alone, for the list) */
template <typename F> void parse_po(const std::string &text, F take) {
    po_entry     e;
    std::string *field = nullptr;
    bool         any   = false, in_str = false, go = true;
    size_t       pos   = 0;

    auto flush = [&]() {
        if (any && go)
            go = take(e);
        e      = po_entry();
        field  = nullptr;
        any    = false;
        in_str = false;
    };
    while (go && pos < text.size()) {
        size_t      nl   = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos              = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const char *l = line.c_str();
        while (*l == ' ' || *l == '\t')
            l++;
        if (!strncmp(l, "#~", 2)) {     /* obsolete: kept in the file for the translator, never read */
            if (in_str)
                flush();
            continue;
        }
        if (*l == '#') {
            if (in_str)
                flush();
            if (l[1] == ',' && strstr(l, "fuzzy"))
                e.fuzzy = true;
            continue;
        }
        if (!*l) {
            if (in_str)
                flush();
            continue;
        }
        if (!strncmp(l, "msgctxt", 7)) {
            if (in_str)
                flush();
            e.has_ctx = any = true;
            field     = &e.ctx;
            unquote(l + 7, *field);
        } else if (!strncmp(l, "msgid_plural", 12)) {
            e.plural = true;
            field    = nullptr;     /* plurals: msgstr[0] is taken, the rest left */
        } else if (!strncmp(l, "msgid", 5)) {
            if (in_str)
                flush();
            any   = true;
            field = &e.id;
            unquote(l + 5, *field);
        } else if (!strncmp(l, "msgstr[0]", 9)) {
            in_str = true;
            field  = &e.str;
            unquote(l + 9, *field);
        } else if (!strncmp(l, "msgstr[", 7)) {
            in_str = true;
            field  = nullptr;
        } else if (!strncmp(l, "msgstr", 6)) {
            in_str = true;
            field  = &e.str;
            unquote(l + 6, *field);
        } else if (*l == '"' && field) {
            unquote(l, *field);
        }
    }
    flush();
}

/* The printf conversions of a text, in order ("d,s,lu,"): a translation must have the same ones */
std::string convs(std::string_view s) {
    std::string out;

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%')
            continue;
        size_t j = i + 1;
        if (j < s.size() && s[j] == '%') {
            i = j;
            continue;
        }
        while (j < s.size() && strchr("-+ #0'", s[j]))
            j++;
        while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '.' || s[j] == '*' || s[j] == '$')) {
            if (s[j] == '*' || s[j] == '$')
                out += s[j];
            j++;
        }
        while (j < s.size() && strchr("hlLqjzt", s[j]))
            out += s[j++];
        out += j < s.size() ? s[j] : '?';
        out += ',';
        i = j;
    }
    return out;
}

std::string read_file(const std::string &path) {
    std::string text;
    FILE       *f = fopen(path.c_str(), "rb");
    char        buf[16384];
    size_t      n;

    if (!f)
        return text;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    fclose(f);
    return text;
}

/* "Key: value" of the header entry */
std::string header_field(const std::string &hdr, const char *key) {
    size_t klen = strlen(key);

    for (size_t pos = 0; pos < hdr.size();) {
        size_t nl = hdr.find('\n', pos);
        std::string_view line(hdr.data() + pos, (nl == std::string::npos ? hdr.size() : nl) - pos);
        pos = nl == std::string::npos ? hdr.size() : nl + 1;
        if (line.size() > klen && !strncasecmp(line.data(), key, klen) && line[klen] == ':') {
            line.remove_prefix(klen + 1);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.remove_suffix(1);
            return std::string(line);
        }
    }
    return "";
}

/* The file of a language: the translator's copy first */
std::string po_path(const char *code, std::string *dir) {
    const char *dirs[] = {I18N_USER_DIR, I18N_DIR};

    for (const char *d : dirs) {
        std::string p = std::string(d) + "/" + code + ".po";
        if (access(p.c_str(), R_OK) == 0) {
            if (dir)
                *dir = d;
            return p;
        }
    }
    return "";
}

bool code_ok(const char *code) {
    if (!*code || strlen(code) >= sizeof(((i18n_lang_t *)0)->code))
        return false;
    for (const char *p = code; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '@')
            return false;
    return true;
}

/* ---- looking a text up ---- */

const char *find(const lang_t *l, std::string_view key) {
    auto it = l->dict.find(key);
    return it == l->dict.end() ? nullptr : it->second;
}

/* An LVGL symbol (LV_SYMBOL_OK: U+F000..U+F8FF, three bytes) at p */
bool symbol_at(std::string_view s, size_t p) {
    return p + 3 <= s.size() && (unsigned char)s[p] == 0xEF && (unsigned char)s[p + 1] >= 0x80 &&
           (unsigned char)s[p + 1] <= 0xA3 && ((unsigned char)s[p + 2] & 0xC0) == 0x80;
}

/* The text itself, or without its colon at the end ("Volume:" as "Volume" + ":") */
const char *find_colon(const lang_t *l, std::string_view core, bool &colon) {
    const char *t = find(l, core);

    colon = false;
    if (!t && core.size() > 1 && core.back() == ':') {
        t     = find(l, core.substr(0, core.size() - 1));
        colon = t != nullptr;
    }
    return t;
}

/* One line: the spaces, brackets and LVGL's symbols at either end kept (" Off ", LV_SYMBOL_OK " Apply", the page
 * button's "(SETTINGS" "1:3)"), the colon too; a "Label: value" line the file lacks whole by its label, the value as
 * it is ("Mode: USB") */
bool line_tr(const lang_t *l, std::string_view line, std::string &out) {
    size_t a = 0, b = line.size();

    for (bool more = true; more && a < b;) {
        more = false;
        while (a < b && (line[a] == ' ' || line[a] == '\t' || line[a] == '('))
            a++, more = true;
        if (symbol_at(line, a))
            a += 3, more = true;
    }
    for (bool more = true; more && b > a;) {
        more = false;
        while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t' || line[b - 1] == ')'))
            b--, more = true;
        if (b >= a + 3 && symbol_at(line, b - 3))
            b -= 3, more = true;
    }
    if (a == b)
        return false;

    std::string_view core = line.substr(a, b - a);
    bool             colon;
    const char      *t    = find_colon(l, core, colon);
    std::string_view rest;

    if (!t) {
        size_t sep = core.find(": ");
        if (sep == std::string_view::npos || sep == 0 || !(t = find(l, core.substr(0, sep))))
            return false;
        colon = true;
        rest  = core.substr(sep + 1);   /* " USB" */
    }
    out.append(line.substr(0, a));
    out.append(t);
    if (colon)
        out.push_back(':');
    out.append(rest);
    out.append(line.substr(b));
    return true;
}

/* A line with LVGL's colour spans ("#cbd5e1 Volume:# #4f8bf5 30#"): the text of each span translated, the codes kept */
bool recolor_tr(const lang_t *l, std::string_view line, std::string &out) {
    if (line.find('#') == std::string_view::npos)
        return false;

    std::string res;
    bool        changed = false;
    size_t      i       = 0;

    while (i < line.size()) {
        size_t h = line.find('#', i);
        if (h == std::string_view::npos) {
            std::string_view rest = line.substr(i);
            if (line_tr(l, rest, res))
                changed = true;
            else
                res.append(rest);
            break;
        }
        std::string_view before = line.substr(i, h - i);
        if (!before.empty()) {
            if (line_tr(l, before, res))
                changed = true;
            else
                res.append(before);
        }
        /* "#rrggbb " opens a span, a lone "#" closes it */
        bool hex = h + 8 <= line.size() && line[h + 7] == ' ';
        for (size_t k = h + 1; hex && k < h + 7; k++)
            hex = isxdigit((unsigned char)line[k]);
        if (hex) {
            res.append(line.substr(h, 8));
            size_t           end   = line.find('#', h + 8);
            std::string_view inner = line.substr(h + 8, end == std::string_view::npos ? std::string_view::npos : end - h - 8);
            if (line_tr(l, inner, res))
                changed = true;
            else
                res.append(inner);
            if (end == std::string_view::npos)
                break;
            res.push_back('#');
            i = end + 1;
            continue;
        }
        res.push_back('#');
        i = h + 1;
    }
    if (changed)
        out.append(res);
    return changed;
}

/* The text line by line; false: no line of it is known */
bool compose(const lang_t *l, const char *s, std::string &out) {
    std::string_view all(s);
    bool             changed = false;

    for (size_t start = 0;;) {
        size_t           nl   = all.find('\n', start);
        std::string_view line = all.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (line_tr(l, line, out) || recolor_tr(l, line, out))
            changed = true;
        else
            out.append(line);
        if (nl == std::string_view::npos)
            break;
        out.push_back('\n');
        start = nl + 1;
    }
    return changed;
}

/* Nothing to translate: no letter (frequencies, levels, times) */
bool wordless(const char *s) {
    for (; *s; s++)
        if (isalpha((unsigned char)*s) || (unsigned char)*s >= 0x80)
            return false;
    return true;
}

/* For a translator, on the PC build: X6100_I18N_MISSING=<file> lists once each text that reached the screen without
 * its translation */
void note_missing(const char *s) {
    static const char *file = getenv("X6100_I18N_MISSING");
    static std::mutex  m;
    static std::unordered_map<std::string, bool> seen;

    if (!file || !*file)
        return;
    std::lock_guard<std::mutex> lock(m);
    if (!seen.emplace(s, true).second)
        return;
    FILE *f = fopen(file, "a");
    if (!f)
        return;
    for (const char *p = s; *p; p++) {
        if (*p == '\n')
            fputs("\\n", f);
        else if (*p == '\t')
            fputs("\\t", f);
        else
            fputc(*p, f);
    }
    fputc('\n', f);
    fclose(f);
}

constexpr int RING_N = 32;      /* texts put together line by line, per thread: LVGL copies them at once */
thread_local std::array<std::string, RING_N> ring;
thread_local int ring_pos;

}  // namespace

extern "C" const char *i18n_tr(const char *s) {
    const lang_t *l = cur.load(std::memory_order_acquire);

    if (!l || !s || !*s || wordless(s))
        return s;

    const char *t = find(l, std::string_view(s));
    if (t)
        return t;

    std::string &out = ring[ring_pos];
    out.clear();
    if (!compose(l, s, out)) {
        note_missing(s);
        return s;
    }
    ring_pos = (ring_pos + 1) % RING_N;
    return out.c_str();
}

extern "C" const char *i18n_tr_static(const char *s) {
    lang_t *l = cur.load(std::memory_order_acquire);

    if (!l || !s || !*s || wordless(s))
        return s;

    const char *t = find(l, std::string_view(s));
    if (t)
        return t;

    std::lock_guard<std::mutex> lock(l->perm_mux);
    auto it = l->perm.find(s);
    if (it != l->perm.end())
        return it->second.c_str();

    std::string out;
    if (!compose(l, s, out)) {
        note_missing(s);
        return s;
    }
    return l->perm.emplace(s, std::move(out)).first->second.c_str();
}

extern "C" bool i18n_load(const char *code) {
    std::string dir, path;

    if (!code || !*code || !strcmp(code, "en")) {
        cur.store(nullptr, std::memory_order_release);
        return true;
    }
    if (!code_ok(code) || (path = po_path(code, &dir)).empty()) {
        fprintf(stderr, "i18n: no language file %s.po: English\n", code);
        cur.store(nullptr, std::memory_order_release);
        return false;
    }

    lang_t     *l       = new lang_t;
    std::string text    = read_file(path);
    int         dropped = 0;

    l->code = code;
    parse_po(text, [&](po_entry &e) {
        if (e.id.empty()) {     /* the header */
            l->name   = header_field(e.str, "X-OpenX6X00-Name");
            l->status = header_field(e.str, "X-OpenX6X00-Status");
            l->font   = header_field(e.str, "X-OpenX6X00-Font");
            return true;
        }
        if (e.fuzzy || e.str.empty())
            return true;
        if (convs(e.id) != convs(e.str)) {
            if (dropped++ < 20)
                fprintf(stderr, "i18n: %s.po: \"%.60s\" left out, its %% conversions differ from the English\n",
                        code, e.id.c_str());
            return true;
        }
        const std::string &key = l->store.emplace_back(e.has_ctx ? e.ctx + '\x04' + e.id : e.id);
        const std::string &val = l->store.emplace_back(e.str);
        l->dict[std::string_view(key)] = val.c_str();
        return true;
    });
    if (!l->font.empty()) {
        std::string beside = dir + "/" + l->font, shared = std::string(I18N_FONT_DIR) + "/" + l->font;
        l->font = access(beside.c_str(), R_OK) == 0 ? beside : access(shared.c_str(), R_OK) == 0 ? shared : "";
        if (l->font.empty())
            fprintf(stderr, "i18n: %s.po: its font is in neither %s nor %s\n", code, dir.c_str(), I18N_FONT_DIR);
    }
    fprintf(stderr, "i18n: %s from %s, %zu texts%s\n", code, path.c_str(), l->dict.size(),
            dropped ? ", some left out (above)" : "");
    {
        std::lock_guard<std::mutex> lock(loaded_mux);
        loaded.push_back(l);
    }
    cur.store(l, std::memory_order_release);
    return true;
}

extern "C" const char *i18n_code(void) {
    const lang_t *l = cur.load(std::memory_order_acquire);
    return l ? l->code.c_str() : "";
}

extern "C" const char *i18n_font(void) {
    const lang_t *l = cur.load(std::memory_order_acquire);
    return l && !l->font.empty() ? l->font.c_str() : NULL;
}

extern "C" void i18n_upper(char *out, size_t size, const char *in, bool ascii) {
    size_t               n = 0;
    const unsigned char *p = (const unsigned char *)in;

    if (!size)
        return;
    while (*p) {
        size_t len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 1;
        for (size_t k = 1; k < len; k++)
            if ((p[k] & 0xC0) != 0x80)
                len = 1;    /* not UTF-8: the byte as it is */
        if (n + len >= size)
            break;
        if (len == 1) {
            out[n++] = ascii && *p >= 'a' && *p <= 'z' ? *p - 'a' + 'A' : *p;
        } else if (len == 2) {
            unsigned cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
            if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
                cp -= 0x20;
            else if (cp == 0xFF)
                cp = 0x178;
            else if (cp >= 0x100 && cp <= 0x17F && cp != 0x138 && cp != 0x149 && cp != 0x17F &&
                     ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E) ? (cp & 1) == 0 : (cp & 1) == 1))
                cp -= 1;    /* Latin Extended-A: pairs, the small letter after its capital (ł, š, ő, œ) */
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            memmove(out + n, p, len);
            n += len;
        }
        p += len;
    }
    out[n] = 0;
}

extern "C" int i18n_list(i18n_lang_t *out, int max) {
    const char *dirs[] = {I18N_USER_DIR, I18N_DIR};
    int         n      = 0;

    if (max < 1)
        return 0;
    memset(&out[n], 0, sizeof(out[n]));
    snprintf(out[n].name, sizeof(out[n].name), "English");
    n++;
    for (int d = 0; d < 2; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp)
            continue;
        std::vector<std::string> codes;
        for (struct dirent *de; (de = readdir(dp));) {
            size_t len = strlen(de->d_name);
            if (len > 3 && !strcmp(de->d_name + len - 3, ".po"))
                codes.emplace_back(de->d_name, len - 3);
        }
        closedir(dp);
        std::sort(codes.begin(), codes.end());
        for (const std::string &code : codes) {
            bool dup = !code_ok(code.c_str()) || code == "en";
            for (int k = 0; k < n && !dup; k++)
                dup = code == out[k].code;
            if (dup || n >= max)
                continue;
            i18n_lang_t *x = &out[n++];
            memset(x, 0, sizeof(*x));
            snprintf(x->code, sizeof(x->code), "%s", code.c_str());
            x->user = d == 0;
            parse_po(read_file(std::string(dirs[d]) + "/" + code + ".po"), [&](po_entry &e) {
                if (e.id.empty()) {
                    snprintf(x->name, sizeof(x->name), "%s", header_field(e.str, "X-OpenX6X00-Name").c_str());
                    snprintf(x->status, sizeof(x->status), "%s", header_field(e.str, "X-OpenX6X00-Status").c_str());
                }
                return false;   /* the header comes first: nothing more is needed */
            });
            if (!x->name[0])
                snprintf(x->name, sizeof(x->name), "%s", x->code);
        }
    }
    return n;
}

/* The English names of the languages whose own names are in another script than Latin; a code not here shows as
 * its code */
static const struct { const char *code, *name; } en_names[] = {
    {"zh_CN", "Chinese, simplified"}, {"zh_TW", "Chinese, traditional"}, {"ja", "Japanese"}, {"ko", "Korean"},
    {"ru", "Russian"}, {"uk", "Ukrainian"}, {"bg", "Bulgarian"}, {"sr", "Serbian"}, {"el", "Greek"},
    {"he", "Hebrew"}, {"ar", "Arabic"}, {"th", "Thai"}, {"hi", "Hindi"},
};

/* Letters past Latin-1 (lead bytes from U+0100 up): our own fonts stop at Latin-1 plus a few, a TrueType font
 * behind them comes only with the language that asks for it */
static bool beyond_latin1(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s >= 0xC4)
            return true;
    return false;
}

extern "C" void i18n_list_name(const i18n_lang_t *lang, char *out, size_t size) {
    if (!beyond_latin1(lang->name) || !strcmp(lang->code, i18n_code()) || i18n_font()) {
        snprintf(out, size, "%s", lang->name);
        return;
    }
    for (const auto &e : en_names)
        if (!strcmp(e.code, lang->code)) {
            snprintf(out, size, "%s (%s)", e.name, lang->code);
            return;
        }
    snprintf(out, size, "%s", lang->code);
}
