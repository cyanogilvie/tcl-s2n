#ifndef _S2NINT_H
#define _S2NINT_H
#include "tclstuff.h"
#include <s2n.h>
#include <stdint.h>
#include <inttypes.h>
#include "tip445.h"

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

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BUILD_s2n
#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT
#endif

#define NS	"::s2n"

// s2n.c internal interface <<<
// s2n.c internal interface >>>

EXTERN int S2n_Init _ANSI_ARGS_((Tcl_Interp * interp));

#ifdef __cplusplus
}
#endif

#endif // _S2NINT_H
// vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
