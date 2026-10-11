/* aether_ui_keynames.h -- the key-name vocabulary is GDK's on every backend
 * (gdk_keyval_name: "a", "A", "comma", "exclam", "space"). AppKit, Android
 * and Win32 learn a key as the character it types; this names that
 * character as GDK would, so a handler comparing k == "comma" (or the DSL's
 * key_text, turning it back) sees the same string everywhere. Each backend
 * includes it once.
 */
#ifndef AETHER_UI_KEYNAMES_H
#define AETHER_UI_KEYNAMES_H

#include <stdio.h>

/* The GDK name of the key that types ASCII character c, into out: a letter
 * or digit is itself, punctuation its keysym word. 0 when c types nothing
 * worth naming (a control character, or outside ASCII). */
static int aeui_keyname_for_char(int c, char* out, int outsize) {
    static const char* punct[] = {
        /* ! */ "exclam", "quotedbl", "numbersign", "dollar", "percent",
        "ampersand", "apostrophe", "parenleft", "parenright", "asterisk",
        "plus", "comma", "minus", "period", "slash",
    };
    static const char* punct2[] = {   /* : ; < = > ? @ */
        "colon", "semicolon", "less", "equal", "greater", "question", "at",
    };
    static const char* punct3[] = {   /* [ \ ] ^ _ ` */
        "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave",
    };
    static const char* punct4[] = {   /* { | } ~ */
        "braceleft", "bar", "braceright", "asciitilde",
    };
    const char* n = NULL;
    if (c == ' ') n = "space";
    else if (c >= '!' && c <= '/') n = punct[c - '!'];
    else if (c >= ':' && c <= '@') n = punct2[c - ':'];
    else if (c >= '[' && c <= '`') n = punct3[c - '['];
    else if (c >= '{' && c <= '~') n = punct4[c - '{'];
    if (n) { snprintf(out, (size_t)outsize, "%s", n); return 1; }
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        snprintf(out, (size_t)outsize, "%c", (char)c);
        return 1;
    }
    return 0;
}

#endif
