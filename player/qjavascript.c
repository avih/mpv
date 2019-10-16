#include <assert.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>

#include <quickjs/quickjs.h>

#include "osdep/io.h"
#include "mpv_talloc.h"
#include "common/common.h"
#include "options/m_property.h"
#include "common/msg.h"
#include "common/msg_control.h"
#include "options/m_option.h"
#include "input/input.h"
#include "options/path.h"
#include "misc/bstr.h"
#include "osdep/timer.h"
#include "osdep/threads.h"
#include "osdep/getpid.h"
#include "stream/stream.h"
#include "sub/osd.h"
#include "core.h"
#include "command.h"
#include "client.h"
#include "libmpv/client.h"

// List of builtin modules and their contents as strings.
// All these are generated from player/javascript/*.js
static const char *const builtin_files[][3] = {
    {"@/defaults.js",
#   include "player/javascript/qdefaults.js.inc"
    },
    {0}
};

// Represents a loaded script. Each has its own js state.
struct script_ctx {
    const char *filename;
    struct mpv_handle *client;
    struct MPContext *mpctx;
    struct mp_log *log;
    char *last_error_str;
};

static struct script_ctx *jctx(JSContext *J)
{
    return (struct script_ctx *)JS_GetContextOpaque(J);
}

static mpv_handle *jclient(JSContext *J)
{
    return jctx(J)->client;
}

// iserr as true indicates an error, and if so, str may indicate a reason.
// Internally ctx->last_error_str is never NULL, and empty indicates success.
static void set_last_error(struct script_ctx *ctx, bool iserr, const char *str)
{
    ctx->last_error_str[0] = 0;
    if (!iserr)
        return;
    if (!str || !str[0])
        str = "Error";
    ctx->last_error_str = talloc_strdup_append(ctx->last_error_str, str);
}

#define MAYBESTR(s) ((s) ? (s) : "<N/A>")

#define JOKV(v) (!JS_IsException(v))
#define JOKI(i) ((i) >= 0)
#define JOKS(s) ((s) != (char *)0)

#define JIFV(j, var, fcall, statement) \
    if (JOKV(var = (fcall))) { \
        statement; \
        JS_FreeValue(j, var); \
    }

#define JIFS(j, var, fcall, statement) \
    if (JOKS(var = (fcall))) { \
        statement; \
        JS_FreeCString(j, var); \
    }

#define JEX_BEGIN \
    int jerr = 0;

#define JEX_BEGIN_TA(tavar) \
    int jerr = 0; \
    void *tavar = talloc_new(NULL);

#define JTHROW      do { jerr = 1; goto done; } while (0)
#define JTRYV(expr) do { if (!JOKV(expr)) JTHROW; } while (0)
#define JTRYI(expr) do { if (!JOKI(expr)) JTHROW; } while (0)
#define JTRYS(expr) do { if (!JOKS(expr)) JTHROW; } while (0)

#define JEX_END_ONERR(rv) \
done: \
    if (jerr) \
        return (rv);

#define JEX_END_TA_ONERR(tavar, rv) \
done: \
    talloc_free(tavar); \
    if (jerr) \
        return (rv);


#define JEX_BEGIN_AF(n) \
    int jerr = 0; \
    static const int qn_ = n; \
    int qsan_ = 0; const char* qsa_[n]; \
    int qvan_ = 0; JSValue qva_[n];

#define JTRYV_AF(expr) do { \
    assert(qvan_ < qn_); \
    if (JOKV(qva_[qvan_] = (expr))) \
        qvan_++; \
    else \
        JTHROW; \
} while (0)

#define JTRYS_AF(expr) do { \
    assert(qsan_ < qn_); \
    if (JOKS(qsa_[qsan_] = (expr))) \
        qsan_++; \
    else \
        JTHROW; \
} while (0)

#define JEX_END_ONERR_AF(rv) \
done: \
    while (qsan_ > 0) \
        JS_FreeCString(J, qsa_[--qsan_]); \
    while (qvan_ > 0) \
        JS_FreeValue(J, qva_[--qvan_]); \
    if (jerr) \
        return (rv);


// typedef struct qaf_val { JSContext *J; JSValue v; } qaf_val;
// typedef struct qaf_cstr { JSContext *J; const char *s; } qaf_cstr;

// static void destruct_af_qval(void *p)
// {
//     qaf_val *pq = (qaf_val*)p;
//     JS_FreeValue(pq->J, pq->v);
// }
// static void add_af_qval(void *parent, JSContext *J,  JSValue v)
// {
//     qaf_val *pq = talloc(parent, qaf_val);
//     pq->J = J;
//     pq->v = v;
//     talloc_set_destructor(pq, destruct_af_qval);
// }

// static void destruct_af_qcstr(void *p)
// {
//     qaf_cstr *pq = (qaf_val*)p;
//     JS_FreeCString(pq->J, pq->s);
// }
// static void add_af_qcstr(void *parent, JSContext *J,  const char *s)
// {
//     qaf_cstr *pq = talloc(parent, qaf_cstr);
//     pq->J = J;
//     pq->s = s;
//     talloc_set_destructor(pq, destruct_af_qcstr);
// }


#define appendf(ptr, ...) \
    do {(*(ptr)) = talloc_asprintf_append_buffer(*(ptr), __VA_ARGS__);} while(0)

static void jerr_to_last_error(struct script_ctx *ctx, JSContext *J)
{
    JSValue exception_val, val;

    set_last_error(ctx, 1, "unknown JavaScript error");
    if (!JOKV(exception_val = JS_GetException(J)))
        return;

    const char *s;
    BOOL is_error, got_stack = 0;
    char *msg = talloc_strdup(NULL, "");

    is_error = JS_IsError(J, exception_val);
    if (!is_error)
        appendf(&msg, "Throw: ");

    s = JS_ToCString(J, exception_val);
    appendf(&msg, "%s", MAYBESTR(s));
    if (s)
        JS_FreeCString(J, s);

    if (is_error) {
        if (JOKV(val = JS_GetPropertyStr(J, exception_val, "stack"))) {
            if (!JS_IsUndefined(val) && JOKS(s = JS_ToCString(J, val))) {
                appendf(&msg, "\n%s", s);
                got_stack = 1;
                JS_FreeCString(J, s);
            }
            JS_FreeValue(J, val);
        }
    }
    if (!got_stack)
        appendf(&msg, "\n    <N/A>\n");

    set_last_error(ctx, 1, msg);
    talloc_free(msg);
    JS_FreeValue(J, exception_val);
}

static JSValue script_log(JSContext *J, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue rv = JS_EXCEPTION;
    const char *level = JS_ToCString(J, argv[0]);
    int msgl = level ? mp_msg_find_level(level) : -1;

    if (msgl >= 0) {
        struct mp_log *log = jctx(J)->log;
        for (int i = 1; i < argc; i++) {
            const char *s = JS_ToCString(J, argv[i]);
            mp_msg(log, msgl, (i == 1 ? "%s" : " %s"), MAYBESTR(s));
            if (s)
                JS_FreeCString(J, s);
        }
        mp_msg(log, msgl, "\n");
        rv = JS_TRUE;
    } else {
        JS_ThrowReferenceError(J, "bad log level: %s", MAYBESTR(level));
    }

    if (level)
        JS_FreeCString(J, level);
    return rv;
}

static const JSCFunctionListEntry mp_funcs[] = {
    JS_CFUNC_DEF("log", 1, script_log ),
};

static int s_init_js(struct script_ctx *ctx, JSContext *J) {
    JEX_BEGIN;
//    static const char *prog = "function x1() { log('hello') }; x1()";
    static const char *prog = "\
//    throw(1);\n\
    log('info', null, Object.create(null, {}), undefined, {a:1}, [2, 3], 'str', {toString: undefined}, 4 );\n\
    log('warn', 1, 2, 3, 'str');\n\
    log('fatal', 1, 2, 3, 'str');\n\
    log('foo', 1, 2, 3, 'str');\n\
    // function x1() {\n\
    //     var a=1;\n\
    //     xlog('hello');\n\
    // };\n\
    // function x2() {\n\
    //     var lala=12;\n\
    //     x1();\n\
    // };\n\
    // throw(1);\n\
    // throw(Error(\n\
    //     {\n\
    //         toString: function() { \n\
    //             return lalala;\n\
    //         }\n\
    //     }\n\
    // ));\n\
    // x2();\n\
    ";
    JS_SetContextOpaque(J, ctx);

    JSValue g;
    JTRYV(g = JS_GetGlobalObject(J));
    JS_SetPropertyFunctionList(J, g, mp_funcs, 1);  // void, impossible OOM?
    JS_FreeValue(J, g);

    //JTRY(fn = JS_NewCFunction(J, script_log, "log", 1), "failed to insert log fn");
    //JS_SetPropertyStr(J, global_obj, "log", fn);
    JTRYV(JS_Eval(J, prog, strlen(prog), "test-prog", JS_EVAL_TYPE_GLOBAL));

    int msgl = mp_msg_find_level("warn");
    mp_msg(jctx(J)->log, msgl, "%s\n", "done1");

done:
    if (jerr)
        jerr_to_last_error(ctx, J);
    return jerr;
}

static int s_load_javascript(struct mpv_handle *client, const char *fname)
{
    struct script_ctx *ctx = talloc_ptrtype(NULL, ctx);
    *ctx = (struct script_ctx) {
        .client = client,
        .mpctx = mp_client_get_core(client),
        .log = mp_client_get_log(client),
        .last_error_str = talloc_strdup(ctx, "Cannot initialize JavaScript"),
        .filename = fname,
    };

    int r = -1;
    JSRuntime *jrt = NULL;
    JSContext *J = NULL;
    if (!(jrt = JS_NewRuntime()) ||
        !(J = JS_NewContext(jrt)) ||
        s_init_js(ctx, J))
    {
        goto error_out;
    }

    set_last_error(ctx, 0, NULL);
    // if (js_pcall(J, 0)) {  // script__run_script
    //     s_top_to_last_error(ctx, J);
    //     goto error_out;
    // }

    r = 0;

error_out:
    if (r)
        MP_FATAL(ctx, "%s\n", ctx->last_error_str);
    if (J)
        JS_FreeContext(J);
    if (jrt)
        JS_FreeRuntime(jrt);

    talloc_free(ctx);
    return r;
}

// main export of this file, used by cplayer to load js scripts
const struct mp_scripting mp_scripting_js = {
    .name = "javascript",
    .file_ext = "js",
    .load = s_load_javascript,
};
