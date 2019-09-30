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

static int s_init_js(JSContext *J, struct script_ctx *ctx) {
    JS_SetContextOpaque(J, ctx);
    return 0;
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
        s_init_js(J, ctx))
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
