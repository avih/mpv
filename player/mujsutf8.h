/*
 * This file wraps all mujs APIs which take or return strings and makes them
 * fully UTF-8. Simply include it after mujs.h and you're good.
 *
 * Strings from mujs are null-terminated CESU-8. Strings sent to mujs are CESU-8
 * either null-terminated or lstring.
 *
 * For mujs APIs which take or return strings, e.g. some js_foo, this file
 * defines the variants u_js_foo and c_js_foo, where the c_ variant is a direct
 * wrapper of js_foo, and the u_ variant exposes a UTF-8 API which converts
 * inputs from UTF-8 to CESU-8 (if required), and converts return values from
 * CESU-8 to UTF-8 (if required).
 *
 * It then defines js_foo as u_js_foo such that the normal names become UTF-8
 * APIs, while the CESU-8 API is still available via the c_ variant if needed.
 */

#ifndef U8J_NO_INCLUDE
    #include <stdio.h>   /* fopen etc */
    #include <stdlib.h>  /* size_t, malloc, realloc, free */
    #include <stdarg.h>  /* for the js_<name>error functions */
#endif


#define U8J_VERSION_MAJOR 1
#define U8J_VERSION_MINOR 0


/* define mujs.h macros if needed in case mujs.h changes in the future */

#ifndef JS_NORETURN
    /* copied verbatim from mujs.h version 1.0.6 */

    /* noreturn is a GCC extension */
    #ifdef __GNUC__
    #define JS_NORETURN __attribute__((noreturn))
    #else
    #ifdef _MSC_VER
    #define JS_NORETURN __declspec(noreturn)
    #else
    #define JS_NORETURN
    #endif
    #endif
#endif

#ifndef JS_PRINTFLIKE
    /* copied verbatim from mujs.h version 1.0.6 */

    /* GCC can do type checking of printf strings */
    #ifdef __printflike
    #define JS_PRINTFLIKE __printflike
    #else
    #if __GNUC__ > 2 || __GNUC__ == 2 && __GNUC_MINOR__ >= 7
    #define JS_PRINTFLIKE(fmtarg, firstvararg) \
        __attribute__((__format__ (__printf__, fmtarg, firstvararg)))
    #else
    #define JS_PRINTFLIKE(fmtarg, firstvararg)
    #endif
    #endif
#endif


#ifdef __GNUC__
    #define U8J__UNUSED __attribute__ ((unused))
#else
    #define U8J_UNUSED
#endif

#define U8J_API U8J__UNUSED static


/*
 * lengths/sizes should be size_t, but mujs APIs use only int, and so do we for
 * compatibility, but make it obvious where size_t should have been used.
 * size_t is still used by U8J if it doesn't necessarily end up at mujs APIs.
 */
typedef int U8J_SIZ;

/*
 * internal alloc functions.
 *
 * Used when returning a string which _is_ converted to UTF-8 (e.g. maybe with
 * js_tostring), or when sending a string which _is_ converted to CESU-8 (e.g.
 * maybe at js_pushstring) _and_ it doesn't fit in U8J_IMPL_STACK_BUF_SIZ.
 */
static void* u8j__malloc(js_State *J, U8J_SIZ siz);
static void  u8j__free(js_State *J, void *ptr);

#ifndef U8J_IMPL_STACK_BUF_SIZ
    #define U8J_IMPL_STACK_BUF_SIZ 256
#endif


#define U8J__DBG(...) \
    do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)


/**************************************************
 * CESU-8 and UTF-8 test and conversion functions *
 **************************************************/

/*
 * Unicode supplementary codepoint is U+10000 or higher. In UTF-8 it's a 4-bytes
 * sequence and in CESU-8 it's 6 (pair of 3-bytes surrogates). Other codepoints
 * are encoded as identical sequences in UTF-8 and CESU-8.
 *
 * CP >= U+10000 in UTF-8: 11110ccc 10ccbbbb 10bbbbaa 10aaaaaa
 * The top 5 bits (ccccc) must be non-zero and equal-to-or-less-than 0x10
 *
 * CP >= U+10000 in CESU-8: 11101101 1010yyyy 10bbbbbb 11101101 1011bbaa 10aaaaaa
 * the CP lower 16 bits are bb...aa, the top 5 are yyyy + 1
 */


/* tests up to 6 bytes, aborts crrectly on string termination */
static inline int u8j__is_cesu8_smp(const unsigned char *s)
{
    return s[0] == 0xed && (s[1] & 0xf0) == 0xa0 && (s[2] & 0xc0) == 0x80 &&
           s[3] == 0xed && (s[4] & 0xf0) == 0xb0 && (s[5] & 0xc0) == 0x80;
}

/* conversion can be in-place if src and dst are the same (utf8 is shorter) */
U8J_API void u8j_write_utf8(const char *cesu8_src, char *utf8_dst)
{
    const unsigned char *s = (const unsigned char *)cesu8_src;
    unsigned char *dst = (unsigned char *)utf8_dst;
    unsigned char top5;

    while (*s) {
        if (u8j__is_cesu8_smp(s)) {
            top5 = (s[1] & 0x0f) + 1;

            dst[0] = 0xf0 | top5 >> 2;
            dst[1] = 0x80 | (top5 & 0x03) << 4 | (s[2] & 0x3f) >> 2;
            dst[2] = 0x80 | (s[2] & 0x03) << 4 | (s[4] & 0x0f);
            dst[3] = s[5];

            s += 6;
            dst += 4;

        } else {
            *dst++ = *s++;
        }
    }

    *dst = 0;
}

/* 0 if no conversion to UTF-8 is required, else the expected UTF-8 strlen */
U8J_API size_t u8j_utf8_len(const char *cesu8)
{
    size_t i = 0, smp_count = 0;
    if (!cesu8)
        return 0;  /* no conversion required */

    for (; cesu8[i]; i++) {
        if (u8j__is_cesu8_smp((const unsigned char *)cesu8 + i))
            smp_count++;
    }

    return smp_count ? i - smp_count * 2 : 0;
}


/*
 * returns a utf8 version of cesu8: itself, or converted into buf if fits, or
 * converted into allocated mem. *mem set to 0 if not alloced, else the alloc.
 * alloc may throw (with *mem=0), else it should be released with u8j__free.
 */
U8J__UNUSED static const char * u8j__as_utf8(js_State *J, const char *cesu8, char *buf, size_t bufsiz, char **mem)
{
    size_t n = u8j_utf8_len(cesu8);
    *mem = NULL;  /* ensure NULL also if u8j__malloc throws */

    if (n == 0)
        return cesu8;

    if (n < bufsiz) {
        u8j_write_utf8(cesu8, buf);
        return buf;
    }

    *mem = u8j__malloc(J, n + 1);
    u8j_write_utf8(cesu8, *mem);
    return *mem;
}


/* tests up to 4 bytes, aborts crrectly on string termination */
static inline int u8j__is_utf8_smp(const unsigned char *s)
{
    return (s[0] & 0xf8) == 0xf0 &&
           (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80 &&
           !(s[0] & 0x04) != !((s[0] & 0x03) | (s[1] & 0x30)) /* top5: 0x01..0x10 */;
}

/*
 * writes cesu8_len+1 bytes to cesu8_dst (adds '\0').
 * cesu8_len must not be 0, and must come from js_[l]cesu8_len(..)
 */
U8J_API void u8j_write_cesu8(const char *utf8_src, char *cesu8_dst, size_t cesu8_len)
{
    const unsigned char *s = (const unsigned char *)utf8_src;
    unsigned char *dst = (unsigned char *)cesu8_dst;
    unsigned char * const fin = dst + cesu8_len;
    unsigned char top5;

    while (dst <= fin - 6) {
        if (u8j__is_utf8_smp(s)) {
            top5 = (s[0] & 0x07) << 2 | (s[1] & 0x30) >> 4;

            dst[0] = 0xed;
            dst[1] = 0xa0 | (top5 - 1);
            dst[2] = 0x80 | (s[1] & 0x0f) << 2 | (s[2] & 0x30) >> 4;

            dst[3] = 0xed;
            dst[4] = 0xb0 | (s[2] & 0x0f);
            dst[5] = s[3];

            s += 4;
            dst += 6;

        } else {
            *dst++ = *s++;
        }
    }
    while (dst < fin)
        *dst++ = *s++;

    *dst = 0;
}

/* 0 if no conversion to CESU-8 is required, else the expected CESU-8 strlen */
U8J_API int u8j_has_utf8_smp(const char * const utf8)
{
    if (utf8) {
        for (int i = 0; utf8[i]; i++) {
            if (u8j__is_utf8_smp((const unsigned char *)utf8 + i))
                return 1;
        }
    }
    return 0;
}

/* 0 if no conversion to CESU-8 is required, else the expected CESU-8 strlen */
U8J_API size_t u8j_cesu8_len(const char * const utf8)
{
    size_t i = 0, smp_count = 0;
    if (!utf8)
        return 0;

    for (; utf8[i]; i++) {
        if (u8j__is_utf8_smp((const unsigned char *)utf8 + i))
            smp_count++;
    }

    /* currently we don't check if the addition wraps-around */
    return smp_count ? i + smp_count * 2 : 0;
}

U8J_API size_t u8j_lcesu8_len(const char *utf8, size_t const utf8_len)
{
    size_t i = 0, smp_count = 0;
    /* we allow NULL utf8 if utf8_len implies an actual buffer */
    if (utf8_len < 4)
        return 0;

    for (; i <= utf8_len - 4; i++) {
        if (u8j__is_utf8_smp((const unsigned char *)utf8 + i))
            smp_count++;
    }

    return smp_count ? utf8_len + smp_count * 2 : 0;
}

/*
 * returns a cesu8 version of utf8: itself, or converted into buf if fits, or
 * converted into allocated mem. *mem set to 0 if not alloced, else the alloc.
 * alloc may throw (with *mem=0), else it should be released with u8j__free.
 */
static const char * u8j__as_cesu8(js_State *J, const char *utf8, char *buf, size_t bufsiz, char **mem)
{
    size_t n = u8j_cesu8_len(utf8);
    *mem = NULL;

    if (n == 0)
        return utf8;

    if (n < bufsiz) {
        u8j_write_cesu8(utf8, buf, n);
        return buf;
    }

    *mem = u8j__malloc(J, n + 1);
    u8j_write_cesu8(utf8, *mem, n);
    return *mem;
}


/***************************************************
 * Wrappers for mujs APIs which take input strings *
 ***************************************************/

/*
 * Direct c_ variant wrapper and plain u_ UTF8 input wrappers:
 *   if no conversion is required - just invoke the cesu8 function,
 *   else if possible convert into a buffer and invoke, else convert into
 *   allocated space, try-invoke the cesu8 func, free the space.
 */

#define U8J__IN_NON(name, proto_args, str_var, call_args) \
    U8J_API void c_js_ ## name proto_args { js_ ## name call_args; } \
    U8J_API void u_js_ ## name proto_args { \
        char buf[U8J_IMPL_STACK_BUF_SIZ]; \
        char *mem; \
        str_var = u8j__as_cesu8(J, str_var, buf, sizeof buf, &mem); \
        if (mem == 0) { \
            js_ ## name call_args; \
            return; \
        } \
        char * volatile cesu8_str = mem; \
        if (js_try(J)) { \
            u8j__free(J, cesu8_str); \
            js_throw(J); \
        } \
        js_ ## name call_args; \
        js_endtry(J); \
        u8j__free(J, cesu8_str); \
    }

#define U8J__IN_INT(name, proto_args, str_var, call_args) \
    U8J_API int c_js_ ## name proto_args { return js_ ## name call_args; } \
    U8J_API int u_js_ ## name proto_args { \
        char buf[U8J_IMPL_STACK_BUF_SIZ]; \
        char *mem; \
        str_var = u8j__as_cesu8(J, str_var, buf, sizeof buf, &mem); \
        if (mem == 0) \
            return js_ ## name call_args; \
        \
        int ret_val; \
        char * volatile cesu8_str = mem; \
        if (js_try(J)) { \
            u8j__free(J, cesu8_str); \
            js_throw(J); \
        } \
        ret_val = js_ ## name call_args; \
        js_endtry(J); \
        u8j__free(J, cesu8_str); \
        return ret_val; \
    }

/* {do,[p]load}string: source is converted, filename is not, maybe it should? */
U8J__IN_INT(dostring, (js_State *J, const char *source), source, (J, source))
U8J__IN_NON(loadstring, (js_State *J, const char *filename, const char *source), source, (J, filename, source))
U8J__IN_INT(ploadstring, (js_State *J, const char *filename, const char *source), source, (J, filename, source))

U8J__IN_NON(getglobal, (js_State *J, const char *name), name, (J, name))
U8J__IN_NON(setglobal, (js_State *J, const char *name), name, (J, name))
U8J__IN_NON(delglobal, (js_State *J, const char *name), name, (J, name))
U8J__IN_NON(defglobal, (js_State *J, const char *name, int atts), name, (J, name, atts))

U8J__IN_INT(hasproperty, (js_State *J, int idx, const char *name), name, (J, idx, name))
U8J__IN_NON(getproperty, (js_State *J, int idx, const char *name), name, (J, idx, name))
U8J__IN_NON(setproperty, (js_State *J, int idx, const char *name), name, (J, idx, name))
U8J__IN_NON(defproperty, (js_State *J, int idx, const char *name, int atts), name, (J, idx, name, atts))

U8J__IN_NON(delproperty, (js_State *J, int idx, const char *name), name, (J, idx, name))
U8J__IN_NON(defaccessor, (js_State *J, int idx, const char *name, int atts), name, (J, idx, name, atts))

U8J__IN_NON(newcfunction, (js_State *J, js_CFunction fun, const char *name, int length), name, (J, fun, name, length))
U8J__IN_NON(newcconstructor, (js_State *J, js_CFunction fun, js_CFunction con, const char *name, int length), name, (J, fun, con, name, length))
U8J__IN_NON(newregexp, (js_State *J, const char *pattern, int flags), pattern, (J, pattern, flags))

U8J__IN_NON(newstring, (js_State *J, const char *v), v, (J, v))


#undef U8J__IN_NON
#undef U8J__IN_INT


/* pushstring variants */

/* util: convert utf8 to cesu8 with known result length and push it */
static void u8j__pushslcesu8string(js_State *J, const char *utf8, int cesu8_len) {
    char buf[U8J_IMPL_STACK_BUF_SIZ];
    if (cesu8_len < sizeof buf) {
        u8j_write_cesu8(utf8, buf, cesu8_len);
        js_pushlstring(J, buf, cesu8_len);
        return;
    }

    char * volatile mem = u8j__malloc(J, cesu8_len + 1);
    u8j_write_cesu8(utf8, mem, cesu8_len);

    if (js_try(J)) {
        u8j__free(J, mem);
        js_throw(J);
    }
    js_pushlstring(J, mem, cesu8_len);
    js_endtry(J);

    u8j__free(J, mem);
}

U8J_API void c_js_pushlstring(js_State *J, const char *v, int n) { js_pushlstring(J, v, n); }
U8J_API void u_js_pushlstring(js_State *J, const char *v, int n) {
    U8J_SIZ cesu8_len = u8j_lcesu8_len(v, n);
    if (cesu8_len == 0)
        js_pushlstring(J, v, n);
    else
        u8j__pushslcesu8string(J, v, cesu8_len);
}

U8J_API void c_js_pushstring(js_State *J, const char *v) { js_pushstring(J, v); }
U8J_API void u_js_pushstring(js_State *J, const char *v) {
    U8J_SIZ cesu8_len = u8j_cesu8_len(v);
    if (cesu8_len == 0)
        js_pushstring(J, v);
    else
        u8j__pushslcesu8string(J, v, cesu8_len);
}

U8J_API void c_js_pushliteral(js_State *J, const char *v) { js_pushliteral(J, v); }
U8J_API void u_js_pushliteral(js_State *J, const char *v) {
    /* mujs literals are utf8, and we define that so are ours, else as string */
    U8J_SIZ cesu8_len = u8j_cesu8_len(v);
    if (cesu8_len == 0)
        js_pushliteral(J, v);
    else
        u8j__pushslcesu8string(J, v, cesu8_len);
}


/* js_{c,u}_[new]<name>error */

#define U8J__DERROR(name_err) \
    U8J_API void c_js_new ## name_err(js_State *J, const char *s) { \
        js_new ## name_err(J, s); \
    } \
    U8J_API void u_js_new ## name_err(js_State *J, const char *s) { \
        /* a bit slower than U8J__IN_NON but less code. errors blow anyway */ \
        u_js_pushstring(J, s); /* push as CESU-8 */ \
        js_new ## name_err(J, /* c_ */ js_tostring(J, -1)); \
        js_replace(J, -2); \
    } \
    /* we can't have a tiny c_ wrapper with va_list, so reimplement... */ \
    JS_NORETURN JS_PRINTFLIKE(2,3) \
    U8J_API void c_js_ ## name_err(js_State *J, const char *fmt, ...) { \
        va_list ap; \
        char buf[256];  /* same size which mujs itself uses internally */ \
        va_start(ap, fmt); \
        vsnprintf(buf, sizeof buf, fmt, ap); \
        va_end(ap); \
        js_new ## name_err(J, buf); \
        js_throw(J); \
    } \
    JS_NORETURN JS_PRINTFLIKE(2,3) \
    U8J_API void u_js_ ## name_err(js_State *J, const char *fmt, ...) { \
        va_list ap; \
        char buf[256]; \
        va_start(ap, fmt); \
        vsnprintf(buf, sizeof buf, fmt, ap); \
        va_end(ap); \
        u_js_new ## name_err(J, buf); \
        js_throw(J); \
    }

U8J__DERROR(error)
U8J__DERROR(evalerror)
U8J__DERROR(rangeerror)
U8J__DERROR(referenceerror)
U8J__DERROR(syntaxerror)
U8J__DERROR(typeerror)
U8J__DERROR(urierror)

#undef U8J__DERROR


/*
 * in u_js_[p]loadfile no API inputs or return values are converted, but the
 * file content is assumed to be UTF-8 and therefore converted to CESU-8 like
 * mujs expects it to be.
 * the implenentation of u_js_loadfile is copied verbatim from the mujs source
 * version 1.0.7-pre, with the following changes:
 * - js_loadstring became u_js_loadstring for the actual conversion.
 * - js_malloc and js_free are not public mujs APIs at the time of writing so
 *   they were replaced with u8j__malloc and u8j__free, respectively.
 */
U8J_API void c_js_loadfile(js_State *J, const char *filename) { js_loadfile(J, filename); }
U8J_API void u_js_loadfile(js_State *J, const char *filename)
{
    FILE *f;
    char *s, *p;
    int n, t;

    f = fopen(filename, "rb");
    if (!f) {
        js_error(J, "cannot open file '%s': %s", filename, strerror(errno));
    }

    if (fseek(f, 0, SEEK_END) < 0) {
        fclose(f);
        js_error(J, "cannot seek in file '%s': %s", filename, strerror(errno));
    }

    n = ftell(f);
    if (n < 0) {
        fclose(f);
        js_error(J, "cannot tell in file '%s': %s", filename, strerror(errno));
    }

    if (fseek(f, 0, SEEK_SET) < 0) {
        fclose(f);
        js_error(J, "cannot seek in file '%s': %s", filename, strerror(errno));
    }

    if (js_try(J)) {
        fclose(f);
        js_throw(J);
    }
    s = u8j__malloc(J, n + 1); /* add space for string terminator */
    js_endtry(J);

    t = fread(s, 1, (size_t)n, f);
    if (t != n) {
        u8j__free(J, s);
        fclose(f);
        js_error(J, "cannot read data from file '%s': %s", filename, strerror(errno));
    }

    s[n] = 0; /* zero-terminate string containing file data */

    if (js_try(J)) {
        u8j__free(J, s);
        fclose(f);
        js_throw(J);
    }

    /* skip first line if it starts with "#!" */
    p = s;
    if (p[0] == '#' && p[1] == '!') {
        p += 2;
        while (*p && *p != '\n')
            ++p;
    }

    u_js_loadstring(J, filename, p);  /* content always assumed UTF-8 */

    u8j__free(J, s);
    fclose(f);
    js_endtry(J);
}

U8J_API int c_js_ploadfile(js_State *J, const char *filename) { return js_ploadfile(J, filename); }
U8J_API int u_js_ploadfile(js_State *J, const char *filename)
{
	if (js_try(J))
		return 1;
	u_js_loadfile(J, filename);
	js_endtry(J);
	return 0;
}


/***********************************************
 * Wrappers for mujs APIs which return strings *
 ***********************************************/

/*
 * The main challenge here is where to store the return value such that it
 * remains available to the caller as long as the original CESU-8 value would
 * (i.e. if a conversion is required to begin with).
 *
 * Our general tool is to keep the UTF-8 string at the VM so that it can be
 * garbage-collected normally.
 *
 * There are only 5 APIs which return a string which may need conversion:
 * - js_tostring, js_torepr, js_trystring, js_tryrepr, js_nextiterator.
 *
 * js_tostring and js_torepr replace a value at the stack with the (CESU-8)
 * string, and we simply replace it again with a userdata object which holds
 * the UTF-8 string and return it. The pointer remains valid as long as the
 * userdata object is at the stack (or referenced in general), and otherwise
 * garbage-collected and finalized (freed).
 *
 * js_trystring and js_tryrepr are similar, except they also take a sentinel
 * error string which, on error, we must return (the pointer) as is and without
 * any conversion, so we can't blindly convert the result. These are therefore
 * simple try/catch wrappers around u_js_try{string,repr}.
 *
 * js_nextierator requires a slightly different approach, see its comment.
 */


/* UTF-8 return value userdata handling */

static void u8j__utf8ret_selfValua(js_State *J)
{
    /* if we're ever used as JS string (unlikely), push a valid CESU-8 string */
    u_js_pushstring(J, (const char *)js_touserdata(J, 0, "u8j_ret"));
}

static int u8j__utf8ret_has(js_State *J, void *p, const char *name)
{
    /* this object supports only these two methods with 0 arguments */
    if (strcmp(name, "toString") && strcmp(name, "valueOf"))
        return 0;

    /*
     * maybe could be optimized by storing the instanciated c function at the
     * registry (once for all future utf8 userdata objects), and fetch it from
     * there on any subsequent use - inserting a c function could be expensive.
     * However, we provide these methods for compliance and interoperability
     * but don't really expect this to get used, ever, so KISS for now.
     */
    js_newcfunction(J, u8j__utf8ret_selfValua, "selfValue", 0);
    return 1;
}


/* push a VM-managed userdata which holds the resulting alloced UTF-8 value */
static const char *u8j__push_utf8ret(js_State *J, const char *cesu8, U8J_SIZ utf8_len)
{
    char * volatile mem = u8j__malloc(J, utf8_len + 1);
    u8j_write_utf8(cesu8, mem);

    if (js_try(J)) {
        u8j__free(J, mem);
        js_throw(J);
    }
    js_pushnull(J);  /* prototype of our object */
    js_newuserdatax(J, "u8j_ret", mem, u8j__utf8ret_has, NULL, NULL, u8j__free);
    js_endtry(J);

    return mem;  /* the VM will manage it from here using the u8j__free dtor */
}

/* c/u variants of js_to<name> and js_try<name> */
#define U8J__OUT_TO_TRY(name) \
    U8J_API const char *c_js_to ## name(js_State *J, int idx) { return js_to ## name(J, idx); } \
    U8J_API const char *u_js_to ## name(js_State *J, int idx) { \
        const char *str = js_to ## name(J, idx); \
        U8J_SIZ utf8_len = u8j_utf8_len(str); \
        if (utf8_len == 0) \
            return str; \
        \
        /* replace idx with a utf8 userdata and return the utf8 pointer */ \
        str = u8j__push_utf8ret(J, str, utf8_len); \
        js_replace(J, idx < 0 ? idx - 1 : idx); \
        return str; \
    } \
    \
    U8J_API const char *c_js_try ## name(js_State *J, int idx, const char *error) { return js_try ## name(J, idx, error); } \
    U8J_API const char *u_js_try ## name(js_State *J, int idx, const char *error) { \
        const char *s; \
        if (js_try(J)) { \
            js_pop(J, 1);  /* the JS error value */ \
            return error; \
        } \
        s = u_js_to ## name(J, idx); \
        js_endtry(J); \
        return s; \
    }


U8J__OUT_TO_TRY(string)
U8J__OUT_TO_TRY(repr)


/*
 * js_nextiterator
 *
 * The challenge here is that we need to keep the iterator object at the stack
 * for the next js_nextiterator call, so we can't replace it with a userdata,
 * and we can't modify the stack either (e.g. push a utf8 userdata and return
 * its value) because this can mess with the caller's code which could use
 * negative index without realizing that the stack grew.
 *
 * Our solution is to store the UTF-8 userdata (if conversion is required) as
 * a property "u8j_ret" of the iterator object, which stays referenced until
 * we replace it with the next one, or until the iterator is garbage-collected.
 */

U8J_API const char * c_js_nextiterator(js_State *J, int idx) { return js_nextiterator(J, idx); }
U8J_API const char * u_js_nextiterator(js_State *J, int idx) {
    const char *str = js_nextiterator(J, idx);
    int utf8_len = u8j_utf8_len(str);
    if (utf8_len == 0)
        return str;

    str = u8j__push_utf8ret(J, str, utf8_len);
    js_setproperty(J, idx < 0 ? idx - 1 : idx, "u8j_ret");
    return str;
}


/*
 * js_report
 *
 * report is a user-provided callback which we want to be called with UTF-8
 * messages, however, it's used by mujs for both non-critical reports and
 * critical reports like OOM, so we can't affford a wrapper which could hit the
 * the same issues itself. If the user really wants, they can use the
 * conversion utilities - at the time of writing things which could need
 * conversion are file names and function names in warnings.
 */


/**********************************************************************
 * u8j__malloc, u8j__free, and their setup possibly via u_js_newstate *
 **********************************************************************/

/* js_{malloc,free}, most correct but not available at the time of writing */
#define U8J_IMPL_ALLOC_API 0

/*
 * plain malloc/free, ignoring alloc/actx values which were (if) provided at
 * js_newstate, even if u_js_newstate VM with U8J_ALLOC_NEWSTATE (see below).
 * For debugging/benchmark, but also validly useful if the user doesn't use
 * a custom allocator and/or doesn't care if U8J allocs use plain malloc/free.
 */
#define U8J_IMPL_ALLOC_PLAIN 1

/*
 * saves alloc/actx at u_js_newstate and uses them later. uses a plain realloc
 * if alloc/actx were not provided at [u_]js_newstate or if the VM is without
 * saved values (e.g. created with c_js_newstate).
 * Has code and runtime overheads even when it ends up using plain realloc.
 */
#define U8J_IMPL_ALLOC_NEWSTATE 2


#ifndef U8J_IMPL_ALLOC
    #define U8J_IMPL_ALLOC U8J_IMPL_ALLOC_NEWSTATE
#endif


U8J_API js_State *c_js_newstate(js_Alloc alloc, void *actx, int flags)
{
    return js_newstate(alloc, actx, flags);
}


#if U8J_IMPL_ALLOC == U8J_IMPL_ALLOC_API

    /* not needed technically, but have an implementation for the declaration */
    /* maybe re-arrange and use #define u8j__{malloc,free} js_{malloc,free} */
    static void *u8j__malloc(js_State *J, U8J_SIZ siz)
    {
        return js_malloc(J, siz);
    }
    static void u8j__free(js_State *J, void *ptr)
    {
        js_free(J, ptr);
    }

    /* not needed technically, but for compat with U8J_IMPL_ALLOC_NEWSTATE */
    U8J_API js_State *u_js_newstate(js_Alloc alloc, void *actx, int flags)
    {
        return js_newstate(alloc, actx, flags);
    }

#elif U8J_IMPL_ALLOC == U8J_IMPL_ALLOC_PLAIN

    static void *u8j__malloc(js_State *J, U8J_SIZ siz)
    {
        void *ptr = malloc((size_t)siz);
        if (!ptr) {
            js_report(J, "U8J OOM");
            js_pushliteral(J, "U8J OOM");
            js_throw(J);
        }
        return ptr;
    }

    static void u8j__free(js_State *J, void *ptr)
    {
        free(ptr);
    }

    /* not needed technically, but for compat with U8J_IMPL_ALLOC_NEWSTATE */
    U8J_API js_State *u_js_newstate(js_Alloc alloc, void *actx, int flags)
    {
        return js_newstate(alloc, actx, flags);
    }

#else  /* U8J_IMPL_ALLOC_NEWSTATE */

    typedef struct u8j__alloc {
        js_Alloc alloc;
        void *actx;
    } u8j__alloc;

    static void *u8j__default_alloc(void *actx, void *ptr, U8J_SIZ siz)
    {
        return realloc(ptr, (size_t)siz);
    }

    /* save alloc/actx just before the retval - for use by u8j__free */
    static void *u8j__malloc_base(js_Alloc alloc, void *actx, U8J_SIZ siz)
    {
        u8j__alloc *mem = (u8j__alloc *)alloc(actx, NULL, sizeof(u8j__alloc) + siz);
        if (!mem)
            return mem;
        mem->alloc = alloc;
        mem->actx = actx;
        return mem + 1;
    }

    static void u8j__free(js_State *J, void *ptr)
    {
        /* the actual alloced mem and alloc/actx are just before ptr */
        u8j__alloc *a = (u8j__alloc *)ptr - 1;
        a->alloc(a->actx, a, 0);
    }

    static void *u8j__malloc(js_State *J, U8J_SIZ siz)
    {
        js_Alloc alloc = u8j__default_alloc;
        void *actx = NULL;

        /*
         * js_getregistry below is the main performance overhead compared to
         * U8J_ALLOC_PLAIN, as the current mujs implementation searches the
         * name at the registry properties tree. However, it's not too bad as
         * the search is without any memory allocations. js_{is,to}userdata are
         * very cheap, and u8j__malloc_base and u8j__free are only negligibly
         * slower than the plain variants. Compared to the performance cost of
         * the actual allocation, this should be very small or even negligible.
         *
         * The memory overhead compared to the plain malloc is two additional
         * pointers which are stored together with each allocation. they're used
         * by u8j__free to release the memory with the same allocator function
         * and context which were used to allocate it.
         *
         * u8j__free therefore does not depend neither on the registry entry
         * itself nor on the userdata object which it holds.
         *
         * Other than being slightly faster to free without js_getregistry, it's
         * mainly important during js_freestate, because there's no guarantee at
         * which order objects are finalized (freed), and u8j__free is also used
         * as a finalizer by U8J for other userdata objects which are released
         * automatically, possibly at js_freestate, and possibly after the
         * registry userdata object was finalized.
         *
         * All these jumps through hoops could be avoided if mujs provided
         * a public API for js_malloc(J, siz) and js_free(J, ptr) - which it has
         * internally, and which we would have used directly instead of our
         * alloc functions. As then we would not need to save the allocator and
         * context at the registry with a js_newstate wrapper, nor would we need
         * to implement our own malloc and free which use them, and there would
         * also be no need for U8J_ALLOC_PLAIN as a simpler alternative.
         *
         * However, at the time of writing there are no public js_{malloc,free}.
         */
        js_getregistry(J, "u8j_alc");
        if (js_isuserdata(J, -1, "u8j_alc")) {
            u8j__alloc *a = (u8j__alloc *)js_touserdata(J, -1, "u8j_alc");
            alloc = a->alloc;
            actx = a->actx;
        }
        js_pop(J, 1);

        void *ptr = u8j__malloc_base(alloc, actx, siz);
        if (!ptr) {
            js_report(J, "U8J OOM");
            js_pushliteral(J, "U8J OOM");
            js_throw(J);
        }
        return ptr;
    }

    static int u8j__save_alloc(js_State *J, js_Alloc alloc, void *actx)
    {
        u8j__alloc * volatile ud = (u8j__alloc *)u8j__malloc_base(alloc, actx, sizeof(u8j__alloc));
        if (!ud)
            return 1;

        /*
         * u8j__malloc_base saves alloc/actx before the pointer, but these are
         * implementation details for u8j__free. save these values also at the
         * "normal" allocated memory which u8j__malloc_base returned and which
         * js_touserdata will return later.
         */
        ud->alloc = alloc;
        ud->actx = actx;

        volatile int ud_success = 0;
        if (js_try(J)) {
            if (!ud_success)
                u8j__free(J, ud);
            return 1;  /* the stack is not restored, but js_freestate follows */
        }

        js_pushnull(J);
        js_newuserdata(J, "u8j_alc", ud, u8j__free);
        ud_success = 1;  /* ud is now VM-managed even if setregistry throws */
        js_setregistry(J, "u8j_alc");

        js_endtry(J);
        return 0;
    }

    U8J_API js_State *u_js_newstate(js_Alloc alloc, void *actx, int flags)
    {
        js_State *J = js_newstate(alloc, actx, flags);

        /*
         * we only need to save alloc/actx at the registry if alloc is provided.
         * u8j__malloc will use u8j__default_alloc if the registry entry is not
         * there, and u8j__free will work correctly regardless of the alloc
         * function which u8j__malloc ended up using.
         */
        if (J && alloc && u8j__save_alloc(J, alloc, actx)) {
            js_freestate(J);
            return NULL;
        }

        return J;
    }

#endif  /* U8J_ALLOC */


#undef U8J_API


#define js_dostring u_js_dostring
#define js_loadstring u_js_loadstring
#define js_ploadstring u_js_ploadstring

#define js_loadfile u_js_loadfile
#define js_ploadfile u_js_ploadfile

#define js_getglobal u_js_getglobal
#define js_setglobal u_js_setglobal
#define js_defglobal u_js_defglobal
#define js_delglobal u_js_delglobal

#define js_hasproperty u_js_hasproperty
#define js_getproperty u_js_getproperty
#define js_setproperty u_js_setproperty
#define js_defproperty u_js_defproperty
#define js_delproperty u_js_delproperty
#define js_defaccessor u_js_defaccessor

#define js_newcfunction u_js_newcfunction
#define js_newcconstructor u_js_newcconstructor
#define js_newregexp u_js_newregexp
#define js_newstring u_js_newstring

#define js_pushlstring u_js_pushlstring
#define js_pushstring u_js_pushstring
#define js_pushliteral u_js_pushliteral

#define js_error u_js_error
#define js_evalerror u_js_evalerror
#define js_rangeerror u_js_rangeerror
#define js_referenceerror u_js_referenceerror
#define js_syntaxerror u_js_syntaxerror
#define js_typeerror u_js_typeerror
#define js_urierror u_js_urierror

#define js_newnewerror u_js_newerror
#define js_newevalerror u_js_newevalerror
#define js_newrangeerror u_js_newrangeerror
#define js_newreferenceerror u_js_newreferenceerror
#define js_newsyntaxerror u_js_newsyntaxerror
#define js_newtypeerror u_js_newtypeerror
#define js_newurierror u_js_newurierror

#define js_tostring u_js_tostring
#define js_trystring u_js_trystring
#define js_torepr u_js_torepr
#define js_tryrepr u_js_tryrepr

#define js_nextiterator u_js_nextiterator

#define js_newstate u_js_newstate
