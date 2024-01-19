#include "s2nInt.h"

// Must be kept in sync with the enum in s2nInt.tcl
static const char* lit_str[L_size] = {
	"",				// L_EMPTY
	"1",			// L_TRUE
	"0",			// L_FALSE
};

TCL_DECLARE_MUTEX(g_init_mutex);
static int				g_init = 0;

// Internal API <<<
void free_interp_cx(ClientData cdata, Tcl_Interp* interp) //<<<
{
	struct interp_cx*	l = (struct interp_cx*)cdata;

	for (int i=0; i<L_size; i++) replace_tclobj(&l->lit[i], NULL);

	ckfree(l);
}

//>>>
// Internal API >>>
// Script API <<<
OBJCMD(push_cmd) //<<<
{
	int			code = TCL_OK;

	enum {A_cmd, A_CHAN, A_args};
	CHECK_MIN_ARGS_LABEL(finally, code, "channelName ?-opt val ...?");

	THROW_ERROR_LABEL(finally, code, "Not implemented yet");

finally:
	return code;
}

//>>>

static struct cmd {
	char*			name;
	Tcl_ObjCmdProc*	proc;
	Tcl_ObjCmdProc*	nrproc;
} cmds[] = {
	{NS "::push",			push_cmd,			NULL},
	{0}
};
// Script API >>>

#ifdef __cplusplus
extern "C" {
#endif
DLLEXPORT int S2n_Init(Tcl_Interp* interp) //<<<
{
	int					code = TCL_OK;
	struct interp_cx*	l = NULL;

#if USE_TCL_STUBS
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL) return TCL_ERROR;
#endif

	Tcl_MutexLock(&g_init_mutex);
	if (!g_init) {
		g_init = 1;
	}
	Tcl_MutexUnlock(&g_init_mutex);

	l = (struct interp_cx*)ckalloc(sizeof *l);
	*l = (struct interp_cx){0};

	for (int i=0; i<L_size; i++)
		replace_tclobj(&l->lit[i], Tcl_NewStringObj(lit_str[i], -1));

	Tcl_Namespace*		ns = Tcl_CreateNamespace(interp, NS, NULL, NULL);
	TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));

	struct cmd*	c = cmds;
	while (c->name) {
		Tcl_Command		r = NULL;
		if (c->nrproc) {
			r = Tcl_NRCreateCommand(interp, c->name, c->proc, c->nrproc, l, NULL);
		} else {
			r = Tcl_CreateObjCommand(interp, c->name, c->proc, l, NULL);
		}
		if (r == NULL) {
			code = TCL_ERROR;
			goto finally;
		}
		c++;
	}

	TEST_OK_LABEL(finally, code, Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION));

	Tcl_SetAssocData(interp, PACKAGE_NAME, free_interp_cx, l);

finally:
	if (code != TCL_OK) {
		if (l) {
			free_interp_cx(l, interp);
			ckfree(l);
			l = NULL;
		}
	}
	return code;
}

//>>>
#if 0
DLLEXPORT int S2n_SafeInit(Tcl_Interp* interp) //<<<
{
	return S2n_Init(interp);
}

//>>>
#endif
#if UNLOAD
DLLEXPORT int S2n_Unload(Tcl_Interp* interp, int flags) //<<<
{
	int			code = TCL_OK;

	Tcl_DeleteAssocData(interp, PACKAGE_NAME);	// Have to do this here, otherwise Tcl will try to call it after we're unloaded

	if (flags == TCL_UNLOAD_DETACH_FROM_PROCESS) {
		Tcl_MutexFinalize(&g_init_mutex);
		g_init_mutex = NULL;
	}

	return code;
}

//>>>
#if 0
DLLEXPORT int S2n_SafeUnload(Tcl_Interp* interp, int flags) //<<<
{
	return S2n_Unload(interp, flags);
}

//>>>
#endif
#endif //UNLOAD
#ifdef __cplusplus
}
#endif

// vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
