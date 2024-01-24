#ifndef _S2NINT_H
#define _S2NINT_H
#include "tclstuff.h"
#include <s2n.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include "tip445.h"
#include "clogs.h"

#ifndef DBG_HANDSHAKE
#define DBG_HANDSHAKE 0
#endif

#ifndef DBG_IO
#define DBG_IO 0
#endif

#ifndef DBG_LIFECYCLE
#define DBG_LIFECYCLE 0
#endif

#undef CLOGS_DBG_PREFIX_FMT
#undef CLOGS_DBG_PREFIX_ARGS
#define CLOGS_DBG_PREFIX_FMT	"[%14.9f %10.3f %s:%d:%s] "
#define CLOGS_DBG_PREFIX_ARGS	, clogs_proctime(), clogs_delta_usec(),  __FILE__, __LINE__, clogs_cstr(__func__, CLOGS_GREENISH)

#define CHECK_S2N(label, var, cmd) \
	if ((cmd) != S2N_SUCCESS) { \
		Tcl_SetErrorCode(interp, "S2N", s2n_strerror_name(s2n_errno), NULL); \
		Tcl_SetObjResult(interp, Tcl_NewStringObj(s2n_strerror(s2n_errno, "EN"), -1)); \
		var = TCL_ERROR; \
		goto label; \
	}

#define CHECK_S2N_POSIX(label, var, cmd) \
	if ((cmd) != S2N_SUCCESS) { \
		if (interp) { \
			Tcl_SetErrorCode(interp, "S2N", s2n_strerror_name(s2n_errno), NULL); \
			Tcl_SetObjResult(interp, Tcl_NewStringObj(s2n_strerror(s2n_errno, "EN"), -1)); \
		} \
		var = EPROTO; \
		goto label; \
	}

// Must match with lit_str[] in s2n.c
enum {
	L_EMPTY,
	L_TRUE,
	L_FALSE,
	L_size
};

struct interp_cx {
	Tcl_Obj*	lit[L_size];
};

struct con_cx {
	struct s2n_connection*	s2n_con;
	Tcl_Channel				basechan;
	s2n_blocked_status		blocked;
	int						handshake_done;
	int						read_closed;
	int						write_closed;
};

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BUILD_s2n
#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT
#endif

#define NS	"::s2n"

// s2n.c internal interface <<<
void register_intrep(Tcl_Obj* obj);
void free_interp_cx(ClientData cdata, Tcl_Interp* interp);
void free_con_cx(struct con_cx* con_cx);
// s2n.c internal interface >>>

EXTERN int S2n_Init _ANSI_ARGS_((Tcl_Interp * interp));

#ifdef __cplusplus
}
#endif

#endif // _S2NINT_H
// vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
