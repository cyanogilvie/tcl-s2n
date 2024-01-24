#include "s2nInt.h"

// Must be kept in sync with the enum in s2nInt.tcl
static const char* lit_str[L_size] = {
	"",				// L_EMPTY
	"1",			// L_TRUE
	"0",			// L_FALSE
};

TCL_DECLARE_MUTEX(g_init_mutex);
static int				g_init = 0;

TCL_DECLARE_MUTEX(g_intreps_mutex);
static Tcl_HashTable	g_intreps;
static int				g_intreps_init = 0;

static int s2n_chan_block_mode(ClientData cdata, int mode);
static int s2n_chan_close2(ClientData cdata, Tcl_Interp* interp, int flags);
static int s2n_chan_input(ClientData cdata, char* buf, int toRead, int* errorCodePtr);
static int s2n_chan_output(ClientData cdata, const char* buf, int toWrite, int* errorCodePtr);
static int s2n_chan_set_option(ClientData cdata, Tcl_Interp* interp, const char* optname, const char* optval);
static int s2n_chan_get_option(ClientData cdata, Tcl_Interp* interp, const char* optname, Tcl_DString* dsPtr);
static void s2n_chan_watch(ClientData cdata, int mask);
static int s2n_chan_handler(ClientData cdata, int mask);

Tcl_ChannelType	s2n_channel_type = {
	.typeName		= "s2n",
	.version		= TCL_CHANNEL_VERSION_5,
	.blockModeProc	= s2n_chan_block_mode,
	.close2Proc		= s2n_chan_close2,
	.inputProc		= s2n_chan_input,
	.outputProc		= s2n_chan_output,
	.setOptionProc	= s2n_chan_set_option,
	.getOptionProc	= s2n_chan_get_option,
	.watchProc		= s2n_chan_watch,
	.handlerProc	= s2n_chan_handler,
};

static int s2n_chan_block_mode(ClientData cdata, int mode) //<<<
{
	struct con_cx*	con_cx = cdata;
	Tcl_DriverBlockModeProc*	base_blockmode = Tcl_ChannelBlockModeProc(Tcl_GetChannelType(con_cx->basechan));
	return base_blockmode(Tcl_GetChannelInstanceData(con_cx->basechan), mode);
}

//>>>
static int s2n_chan_close2(ClientData cdata, Tcl_Interp* interp, int flags) //<<<
{
	int				posixcode = 0;
	struct con_cx*	con_cx = cdata;

	CLOGS(IO, "--> %x", flags);
	if (flags & TCL_CLOSE_READ) {
		if (interp) {
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_chan_close2: Cannot close read side"));
			Tcl_SetErrorCode(interp, "S2N", "CHAN", "CLOSE2", NULL);
		}
		posixcode = EINVAL;
		goto finally;
	} else if (flags & TCL_CLOSE_WRITE && !con_cx->write_closed) {
		s2n_blocked_status	blocked = S2N_NOT_BLOCKED;
		const int rc = s2n_shutdown_send(con_cx->s2n_con, &blocked);
		if (rc == S2N_SUCCESS) {
			con_cx->write_closed = 1;
		} else {
			switch (s2n_error_get_type(s2n_errno)) {
				case S2N_ERR_T_BLOCKED:
				case S2N_ERR_T_CLOSED:
					con_cx->write_closed = 1;
					break;

				case S2N_ERR_T_IO:
					posixcode = errno;
					goto set_interp_err1;

				case S2N_ERR_T_PROTO:
					posixcode = EPROTO;
					goto set_interp_err1;

				default:
					posixcode = EINVAL;
					goto set_interp_err1;

				set_interp_err1:
					if (interp) {
						Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_chan_close2: s2n_shutdown_send failed: %s", s2n_strerror(s2n_errno, "EN")));
						Tcl_SetErrorCode(interp, "S2N", "CHAN", "CLOSE2", NULL);
					}
			}
		}

	} else if (flags == 0) {
		if (con_cx->write_closed) {
			free_con_cx(con_cx);
			con_cx = NULL;
		} else {
			s2n_blocked_status	blocked = S2N_NOT_BLOCKED;
			CLOGS(IO, "calling s2n_shutdown");
			const int rc = s2n_shutdown(con_cx->s2n_con, &blocked);
			// TODO: Handle blocked?
			if (rc == S2N_SUCCESS) {
				free_con_cx(con_cx);
				con_cx = NULL;
			} else {
				switch (s2n_error_get_type(s2n_errno)) {
					case S2N_ERR_T_BLOCKED:
					case S2N_ERR_T_CLOSED:
						free_con_cx(con_cx);
						con_cx = NULL;
						break;

					case S2N_ERR_T_IO:
						posixcode = errno;
						goto set_interp_err2;

					case S2N_ERR_T_PROTO:
						posixcode = EPROTO;
						goto set_interp_err2;

					default:
						posixcode = EINVAL;
						goto set_interp_err2;

					set_interp_err2:
						if (interp) {
							Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_chan_close2: s2n_shutdown_send failed: %s", s2n_strerror(s2n_errno, "EN")));
							Tcl_SetErrorCode(interp, "S2N", "CHAN", "CLOSE2", NULL);
						}
				}
			}
		}
		// TODO: close lower channel?
	} else {
		if (interp) {
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_chan_close2: unknown flags: %d", flags));
			Tcl_SetErrorCode(interp, "S2N", "CHAN", "CLOSE2", NULL);
		}
		posixcode = EINVAL;
		goto finally;
	}

finally:
	CLOGS(IO, "<-- %x returning %d", flags, posixcode);
	return posixcode;
}

//>>>
static int s2n_chan_input(ClientData cdata, char* buf, int toRead, int* errorCodePtr) //<<<
{
	struct con_cx*		con_cx = cdata;
	s2n_blocked_status	blocked = S2N_NOT_BLOCKED;
	int					remain = toRead;
	int					read_total = 0;

	if (con_cx->read_closed) return 0;

	CLOGS(IO, "--> toRead: %d", toRead);
	while (remain) {
		const ssize_t got = s2n_recv(con_cx->s2n_con, buf+read_total, remain, &blocked);
		CLOGS(IO, "\ts2n_recv(%d) got %zd bytes", remain, got);
		if (got > 0) {
			remain -= got;
			read_total += got;
		} else if (got == 0) {
			con_cx->read_closed = 1;
			break;
        } else {
			switch (s2n_error_get_type(s2n_errno)) {
				case S2N_ERR_T_BLOCKED:
					if (read_total == 0) {
						*errorCodePtr = EAGAIN;
						read_total = -1;
					}
					break;
				case S2N_ERR_T_CLOSED:
					con_cx->read_closed = 1;
					break;
				default:
					fprintf(stderr, "\ts2n_chan_input: s2n_recv failed: %s, errno: %d\n", s2n_strerror(s2n_errno, "EN"), errno);
					*errorCodePtr = errno ? errno : EIO;
					read_total = -1;
					break;
			}
			break;
		}
	}

	CLOGS(IO, "<-- toRead: %d returning %d", toRead, read_total);
	return read_total;
}

//>>>
static int s2n_chan_output(ClientData cdata, const char* buf, int toWrite, int* errorCodePtr) //<<<
{
	struct con_cx*		con_cx = cdata;
	s2n_blocked_status	blocked = S2N_NOT_BLOCKED;
    int					bytes_written = 0;
	int					remain = toWrite;

	if (con_cx->write_closed) {
		CLOGS(IO, "write closed");
		*errorCodePtr = EPIPE;
		return -1;
	}

	CLOGS(IO, "--> toWrite: %d", toWrite);
	if (con_cx->handshake_done) {
		while (remain) {
			const int	wrote = s2n_send(con_cx->s2n_con, buf+bytes_written, remain, &blocked);
			CLOGS(IO, "\ts2n_send(%d) wrote %d bytes", remain, wrote);
			if (wrote >= 0) {
				bytes_written += wrote;
				remain -= wrote;
			} else if (s2n_error_get_type(s2n_errno) == S2N_ERR_T_BLOCKED && blocked == S2N_BLOCKED_ON_WRITE) {
				if (bytes_written == 0) {
					*errorCodePtr = EAGAIN;
					bytes_written = -1;
				}
				break;
			} else {
				*errorCodePtr = errno;
				bytes_written = -1;
				break;
			}
		}
	} else {
		CLOGS(IO, "handshake not done, returning EAGAIN");
		*errorCodePtr = EAGAIN;
		bytes_written = -1;
	}

	CLOGS(IO, "<-- toWrite: %d returning %d", toWrite, bytes_written);
	return bytes_written;
}

//>>>
static int s2n_chan_set_option(ClientData cdata, Tcl_Interp* interp, const char* optname, const char* optval) //<<<
{
	int				code = TCL_OK;
	struct con_cx*	con_cx = cdata;

	if (strcmp(optname, "-servername") == 0) {
		CHECK_S2N(finally, code, s2n_set_server_name(con_cx->s2n_con, optval));
	} else {
		code = Tcl_BadChannelOption(interp, optname, "servername");
		Tcl_SetErrno(EINVAL);
		goto finally;
	}

finally:
	return code;
}

//>>>
static const char* proto_str(int proto) //<<<
{
	switch (proto) {
		case S2N_SSLv2:		return "SSLv2";
		case S2N_SSLv3:		return "SSLv3";
		case S2N_TLS10:		return "TLSv1.0";
		case S2N_TLS11:		return "TLSv1.1";
		case S2N_TLS12:		return "TLSv1.2";
		case S2N_TLS13:		return "TLSv1.3";
		default:			return "<unknown>";
	}
}

//>>>
static int s2n_chan_get_option(ClientData cdata, Tcl_Interp* interp, const char* optname, Tcl_DString* val) //<<<
{
	int				code = TCL_OK;
	struct con_cx*	con_cx = cdata;

	if (optname == NULL) {
		// Return all optionnames and their current values in val
		Tcl_DStringAppendElement(val, "-servername");
		const char*		servername = s2n_get_server_name(con_cx->s2n_con);
		Tcl_DStringAppendElement(val, servername ? servername : "");

		Tcl_DStringAppendElement(val, "-prefer");
		// TODO: keep a record of this
		Tcl_DStringAppendElement(val, "<todo>");

		Tcl_DStringAppendElement(val, "-server_supports");
		Tcl_DStringAppendElement(val, proto_str(s2n_connection_get_server_protocol_version(con_cx->s2n_con)));

		Tcl_DStringAppendElement(val, "-client_supports");
		Tcl_DStringAppendElement(val, proto_str(s2n_connection_get_client_protocol_version(con_cx->s2n_con)));

		Tcl_DStringAppendElement(val, "-protocol");
		Tcl_DStringAppendElement(val, proto_str(s2n_connection_get_actual_protocol_version(con_cx->s2n_con)));

	} else if (strcmp(optname, "-servername") == 0) {
		const char*		servername = s2n_get_server_name(con_cx->s2n_con);
		if (servername) Tcl_DStringAppend(val, servername, -1);

	} else if (strcmp(optname, "-prefer") == 0) {
		// TODO: keep a record of this
		Tcl_DStringAppend(val, "<todo>", -1);

	} else if (strcmp(optname, "-server_supports") == 0) {
		Tcl_DStringAppend(val, proto_str(s2n_connection_get_server_protocol_version(con_cx->s2n_con)), -1);

	} else if (strcmp(optname, "-client_supports") == 0) {
		Tcl_DStringAppend(val, proto_str(s2n_connection_get_client_protocol_version(con_cx->s2n_con)), -1);

	} else if (strcmp(optname, "-protocol") == 0) {
		Tcl_DStringAppend(val, proto_str(s2n_connection_get_actual_protocol_version(con_cx->s2n_con)), -1);

	} else {
		code = Tcl_BadChannelOption(interp, optname, "servername prefer server_supports client_supports protocol");
		Tcl_SetErrno(EINVAL);
		goto finally;
	}

finally:
	return code;
}

//>>>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
static const char* mask_str(int mask) //<<<
{
	if (mask & TCL_EXCEPTION) {
		if (mask & TCL_READABLE) {
			return (mask & TCL_WRITABLE) ? "TCL_EXCEPTION|TCL_READABLE|TCL_WRITABLE" : "TCL_EXCEPTION|TCL_READABLE";
		} else {
			return (mask & TCL_WRITABLE) ? "TCL_EXCEPTION|TCL_WRITABLE" : "TCL_EXCEPTION";
		}
	} else {
		if (mask & TCL_READABLE) {
			return (mask & TCL_WRITABLE) ? "TCL_READABLE|TCL_WRITABLE" : "TCL_READABLE";
		} else {
			return (mask & TCL_WRITABLE) ? "TCL_WRITABLE" : "0";
		}
	}
}

//>>>
#pragma GCC diagnostic pop
static void s2n_chan_watch(ClientData cdata, int mask) //<<<
{
	struct con_cx*	con_cx = cdata;

	if (!con_cx->handshake_done) {
		// While the handshake is busy, signal that we want to be notified when IO is possible
		mask = 0;
		switch (con_cx->blocked) {
			case S2N_BLOCKED_ON_READ:	mask |= TCL_READABLE; break;
			case S2N_BLOCKED_ON_WRITE:	mask |= TCL_WRITABLE; break;
			default: break;
		}
	}

	Tcl_DriverWatchProc*	base_watch = Tcl_ChannelWatchProc(Tcl_GetChannelType(con_cx->basechan));
	return base_watch(Tcl_GetChannelInstanceData(con_cx->basechan), mask);
}

//>>>
static int s2n_chan_handler(ClientData cdata, int mask) //<<<
{
	struct con_cx*	con_cx = cdata;

	CLOGS(IO, "mask: %s, handshake_done: %d", mask_str(mask), con_cx->handshake_done);
	if (!con_cx->handshake_done) {
		// While the handshake is busy, don't report readable and writable to
		// the users of this channel - the only IO that can happen on the
		// channel is the TLS handshake, handled internally.
		Tcl_DriverWatchProc*	base_watch = Tcl_ChannelWatchProc(Tcl_GetChannelType(con_cx->basechan));
		mask = 0;

		CLOGS(HANDSHAKE, "handshake not done, calling s2n_negotiate");
		con_cx->blocked = S2N_NOT_BLOCKED;
		const int neg_rc = s2n_negotiate(con_cx->s2n_con, &con_cx->blocked);
		if (neg_rc == S2N_SUCCESS) {
			con_cx->handshake_done = 1;
			mask |= TCL_WRITABLE;
			if (s2n_peek(con_cx->s2n_con) > 0) mask |= TCL_READABLE;
		} else {
			switch (s2n_error_get_type(s2n_errno)) {
				case S2N_ERR_T_BLOCKED:
				{
					int internal_mask = 0;
					switch (con_cx->blocked) {
						case S2N_BLOCKED_ON_READ:	internal_mask |= TCL_READABLE; break;
						case S2N_BLOCKED_ON_WRITE:	internal_mask |= TCL_WRITABLE; break;
						default: break;
					}
					if (internal_mask) {
						CLOGS(HANDSHAKE, "handshake blocked on %s, passing on mask: %s", con_cx->blocked == S2N_BLOCKED_ON_WRITE ? "write" : "read", mask_str(internal_mask));
						base_watch(Tcl_GetChannelInstanceData(con_cx->basechan), internal_mask);
					}
					break;
				}

				case S2N_ERR_T_IO:
					fprintf(stderr, "s2n_chan_handler: s2n_negotiate failed: %s, errno: %d\n", s2n_strerror(s2n_errno, "EN"), errno);
					break;

				default:
					fprintf(stderr, "s2n_chan_handler: s2n_negotiate failed: %s\n", s2n_strerror(s2n_errno, "EN"));
					break;
			}
		}
	}

	CLOGS(IO, "returning %s", mask_str(mask));
	return mask;
}

//>>>

// Internal API <<<
void free_interp_cx(ClientData cdata, Tcl_Interp* interp) //<<<
{
	struct interp_cx*	l = (struct interp_cx*)cdata;

	for (int i=0; i<L_size; i++) replace_tclobj(&l->lit[i], NULL);

	ckfree(l);
}

//>>>
void register_intrep(Tcl_Obj* obj) //<<<
{
	Tcl_HashEntry*		he = NULL;
	int					new = 0;

	Tcl_MutexLock(&g_intreps_mutex);
	he = Tcl_CreateHashEntry(&g_intreps, obj, &new);
	if (!new) Tcl_Panic("register_intrep: already registered");
	Tcl_SetHashValue(he, obj);
	Tcl_MutexUnlock(&g_intreps_mutex);
}

//>>>
void forget_intrep(Tcl_Obj* obj) //<<<
{
	Tcl_HashEntry*		he = NULL;

	Tcl_MutexLock(&g_intreps_mutex);
	he = Tcl_FindHashEntry(&g_intreps, obj);
	if (!he) Tcl_Panic("forget_intrep: not registered");
	Tcl_DeleteHashEntry(he);
	Tcl_MutexUnlock(&g_intreps_mutex);
}

//>>>

static void free_s2n_config_intrep(Tcl_Obj* obj);
static void dup_s2n_config_intrep(Tcl_Obj* src, Tcl_Obj* dst);

Tcl_ObjType s2n_config_type = {
	.name			= "::s2n::config",
	.freeIntRepProc	= free_s2n_config_intrep,
	.dupIntRepProc	= dup_s2n_config_intrep,
};

static void free_s2n_config_intrep(Tcl_Obj* obj) //<<<
{
	struct s2n_config*	config = NULL;

	forget_intrep(obj);
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &s2n_config_type);
	if (ir) {
		config = (struct s2n_config*)ir->twoPtrValue.ptr1;
		if (config) {
			s2n_config_free(config);
			config = NULL;
		}
	}
}

//>>>
static void dup_s2n_config_intrep(Tcl_Obj* src, Tcl_Obj* dst) //<<<
{
	int	len;
	const char*	str = Tcl_GetStringFromObj(src, &len);
	Tcl_InitStringRep(dst, str, len);
}

//>>>
static int get_s2n_config_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct s2n_config** config) //<<<
{
	int					code = TCL_OK;
	Tcl_DictSearch		search = {0};
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &s2n_config_type);
	struct s2n_config*	c = NULL;

	if (!ir) {
		Tcl_Obj*		key = NULL;
		Tcl_Obj*		val = NULL;
		int				done;

		c = s2n_config_new();

		TEST_OK_LABEL(finally, code, Tcl_DictObjFirst(interp, obj, &search, &key, &val, &done));
		for (; !done; Tcl_DictObjNext(&search, &key, &val, &done)) {
			static const char* config_names[] = {
				"session_tickets",
				"ticket_lifetime",
				"cipher_preferences",
				NULL
			};
			enum config {
				CONFIG_SESSION_TICKETS,
				CONFIG_TICKET_LIFETIME,
				CONFIG_CIPHER_PREFERENCES,
			} conf_name;
			int conf_name_int;

			TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, key, config_names, "config", TCL_EXACT, &conf_name_int));
			conf_name = conf_name_int;

			switch (conf_name) {
				case CONFIG_SESSION_TICKETS:
				{
					uint8_t	enabled;
					TEST_OK_LABEL(finally, code, Tcl_GetBooleanFromObj(interp, val, &enabled));
					CHECK_S2N(finally, code, s2n_config_set_session_cache_onoff(c, enabled));
					break;
				}

				case CONFIG_TICKET_LIFETIME:
				{
					Tcl_Obj**	ov;
					int			oc;
					Tcl_WideInt	lifetime;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, val, &oc, &ov));
					if (oc != 2) THROW_ERROR_LABEL(finally, code, "ticket_lifetime must be a list of two integers", val);
					TEST_OK_LABEL(finally, code, Tcl_GetWideIntFromObj(interp, ov[0], &lifetime));
					CHECK_S2N(finally, code, s2n_config_set_ticket_encrypt_decrypt_key_lifetime(c, lifetime));
					TEST_OK_LABEL(finally, code, Tcl_GetWideIntFromObj(interp, ov[1], &lifetime));
					CHECK_S2N(finally, code, s2n_config_set_ticket_decrypt_key_lifetime(c, lifetime));
					break;
				}

				case CONFIG_CIPHER_PREFERENCES:
					CHECK_S2N(finally, code, s2n_config_set_cipher_preferences(c, Tcl_GetString(val)));
					break;

				default: THROW_ERROR_LABEL(finally, code, "Unhandled config", key);
			}
		}

		Tcl_GetString(obj);	// Ensure that the string rep is generated before we take over the intrep - we can't generate our own
		Tcl_StoreInternalRep(obj, &s2n_config_type, &(Tcl_ObjInternalRep){.twoPtrValue.ptr1 = c});
		c = NULL;		// Hand ownership to the intrep
		register_intrep(obj);
		ir = Tcl_FetchInternalRep(obj, &s2n_config_type);
	}

	*config = (struct s2n_config*)ir->twoPtrValue.ptr1;

finally:
	Tcl_DictObjDone(&search);
	if (c) {
		if (-1 == s2n_config_free(c)) {
			if (code == TCL_OK) {
				code = TCL_ERROR;
				Tcl_SetErrorCode(interp, "S2N", s2n_strerror_name(s2n_errno), NULL);
				Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_config_free failed: %s", s2n_strerror(s2n_errno, "EN")));
				s2n_errno = S2N_ERR_T_OK;
			}
		}
		c = NULL;
	}
	return code;
}

//>>>
void free_con_cx(struct con_cx* con_cx) //<<<
{
	CLOGS(LIFECYCLE, "free_con_cx");
	if (con_cx->s2n_con) {
		CLOGS(LIFECYCLE, "Freeing s2n connection");
		if (-1 == s2n_connection_free(con_cx->s2n_con)) {
			Tcl_Panic("s2n_connection_free failed: %s\n", s2n_strerror(s2n_errno, "EN"));
		}
		con_cx->s2n_con = NULL;
	}
	ckfree(con_cx); con_cx = NULL;
}

//>>>
static int s2n_basechan_send(void* io_context, const uint8_t* buf, uint32_t len) //<<<
{
	struct con_cx*	con_cx = io_context;
	const int sent = Tcl_WriteRaw(con_cx->basechan, (char*)buf, len);
	CLOGS(IO, "len: %d sent %d bytes", len, sent);
	return sent;
}

//>>>
static int s2n_basechan_recv(void* io_context, uint8_t* buf, uint32_t len) //<<<
{
	struct con_cx*	con_cx = io_context;
	const int got = Tcl_ReadRaw(con_cx->basechan, (char*)buf, len);
	CLOGS(IO, "len %d got %d bytes", len, got);
	return got;
}

//>>>

// Internal API >>>
// Script API <<<
OBJCMD(push_cmd) //<<<
{
	int						code = TCL_OK;
	int						i;
	static const char* opts[] = {
		"-config",
		"-role",
		"-servername",
		"-prefer",
		NULL
	};
	enum opt {
		OPT_CONFIG,
		OPT_ROLE,
		OPT_SERVERNAME,
		OPT_PREFER,
	};
	static const char* s2n_role_str[] = { "client", "server", NULL };
	enum role { ROLE_CLIENT, ROLE_SERVER } role = ROLE_CLIENT;
	int				roleint;
	struct con_cx	*con_cx = NULL;
	int				stacked = 0;

	enum {A_cmd, A_CHAN, A_args};
	CHECK_MIN_ARGS_LABEL(finally, code, "channelName ?-opt val ...?");

	int basemode;
	Tcl_Channel	basechan = Tcl_GetChannel(interp, Tcl_GetString(objv[A_CHAN]), &basemode);
	if (
		(basemode & TCL_READABLE) == 0 ||
		(basemode & TCL_WRITABLE) == 0
	) THROW_ERROR_LABEL(finally, code, "Channel must be readable and writable");

	con_cx = (struct con_cx*)ckalloc(sizeof *con_cx);
	*con_cx = (struct con_cx){
		.basechan	= basechan,
		.blocked	= S2N_NOT_BLOCKED,
	};

	// Need to scan the options first to get the role
	for (i=A_args; i<objc; i++) {
		int			optint;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, objv[i], opts, "option", 0, &optint));
		const enum opt	o = optint;
		switch (o) {
			case OPT_ROLE:
				TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, objv[++i], s2n_role_str, "role", 0, &roleint));
				role = roleint;
				break;

			case OPT_CONFIG:
			case OPT_SERVERNAME:
			case OPT_PREFER:
				i++; break;

			default:
				THROW_ERROR_LABEL(finally, code, "Unhandled option", objv[i]);
		}
	}

	con_cx->s2n_con = s2n_connection_new(role == ROLE_CLIENT ? S2N_CLIENT : S2N_SERVER);

	for (i=A_args; i<objc; i++) {
		int			optint;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, objv[i], opts, "option", 0, &optint));
		const enum opt	o = optint;
		switch (o) {
			case OPT_ROLE:	i++; break;		// Handled above
			case OPT_CONFIG: //<<<
			{
				struct s2n_config*	config = NULL;
				if (i == objc-1) THROW_ERROR_LABEL(finally, code, "Missing value for -config", NULL);
				TEST_OK_LABEL(finally, code, get_s2n_config_from_obj(interp, objv[++i], &config));
				// TODO: need to take a ref on the config obj, in the stack chan's instance data?
				TEST_OK_LABEL(finally, code, s2n_connection_set_config(con_cx->s2n_con, config));
				break;
			}
			//>>>
			case OPT_SERVERNAME: //<<<
				if (i == objc-1) THROW_ERROR_LABEL(finally, code, "Missing value for -servername", NULL);
				CHECK_S2N(finally, code, s2n_set_server_name(con_cx->s2n_con, Tcl_GetString(objv[++i])));
				break;
			//>>>
			case OPT_PREFER: //<<<
			{
				static const char* s2n_prefer_str[] = { "throughput", "latency", NULL };
				enum prefer { PREFER_THROUGHPUT, PREFER_LATENCY } prefer = PREFER_THROUGHPUT;
				int	preferint;

				if (i == objc-1) THROW_ERROR_LABEL(finally, code, "Missing value for -prefer", NULL);
				TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, objv[++i], s2n_prefer_str, "prefer", 0, &preferint));
				prefer = preferint;
				switch (prefer) {
					case PREFER_THROUGHPUT: CHECK_S2N(finally, code, s2n_connection_prefer_throughput(con_cx->s2n_con));  break;
					case PREFER_LATENCY:    CHECK_S2N(finally, code, s2n_connection_prefer_low_latency(con_cx->s2n_con)); break;
					default: THROW_ERROR_LABEL(finally, code, "Unhandled prefer", objv[i]);
				}
				break;
			}
			//>>>
			default: THROW_ERROR_LABEL(finally, code, "Unhandled option", objv[i]);
		}
	}

	// Wire up IO callbacks to read and write to the base chan
	CHECK_S2N(finally, code, s2n_connection_set_send_ctx(con_cx->s2n_con, con_cx));
	CHECK_S2N(finally, code, s2n_connection_set_recv_ctx(con_cx->s2n_con, con_cx));
	CHECK_S2N(finally, code, s2n_connection_set_send_cb(con_cx->s2n_con, s2n_basechan_send));
	CHECK_S2N(finally, code, s2n_connection_set_recv_cb(con_cx->s2n_con, s2n_basechan_recv));

	CLOGS(HANDSHAKE, "s2n_negotiate");
    const int neg_rc = s2n_negotiate(con_cx->s2n_con, &con_cx->blocked);

	if (neg_rc == S2N_SUCCESS) {
		CLOGS(HANDSHAKE, "s2n_negotiate success");
		con_cx->handshake_done = 1;
	} else {
		switch (s2n_error_get_type(s2n_errno)) {
			case S2N_ERR_T_BLOCKED:
			{
				int		mask = 0;
				switch (con_cx->blocked) {
					case S2N_BLOCKED_ON_READ:	mask |= TCL_READABLE; break;
					case S2N_BLOCKED_ON_WRITE:	mask |= TCL_WRITABLE; break;
					default: break;
				}
				if (mask) {
					CLOGS(HANDSHAKE, "s2n_negotiate blocked on %s, registering watch for %s", con_cx->blocked == S2N_BLOCKED_ON_READ ? "read" : "write", mask_str(mask));
					Tcl_DriverWatchProc*	base_watch = Tcl_ChannelWatchProc(Tcl_GetChannelType(con_cx->basechan));
					base_watch(Tcl_GetChannelInstanceData(con_cx->basechan), mask);
				}
				break;
			}

			default:
				THROW_ERROR_LABEL(finally, code, "s2n_negotiate failed: ", s2n_strerror(s2n_errno, "EN"));
		}
	}

	Tcl_Channel tlschan = Tcl_StackChannel(interp, &s2n_channel_type, con_cx, TCL_READABLE | TCL_WRITABLE, basechan);
	CLOGS(HANDSHAKE, "leaving push_cmd, handshake_done: %d", con_cx->handshake_done);
	con_cx = NULL;	// Hand ownershop to the s2n_channel_type driver
	stacked = 1;

finally:
	if (con_cx) {
		free_con_cx(con_cx);
		con_cx = NULL;
	}
	if (code != TCL_OK && stacked) {
		code = Tcl_UnstackChannel(interp, tlschan);
	}
	return code;
}

//>>>
OBJCMD(openssl_version_cmd) //<<<
{
	int			code = TCL_OK;

	enum {A_cmd, A_objc};
	CHECK_ARGS_LABEL(finally, code, "");

	const unsigned long ver = s2n_get_openssl_version();

	Tcl_SetObjResult(interp, Tcl_ObjPrintf("%d.%d.%d.%d",
				(int)((ver >> 28) & 0x0f),
				(int)((ver >> 20) & 0xff),
				(int)((ver >> 12) & 0xff),
				(int)( ver        & 0x0f)));

finally:
	return code;
}

//>>>

static struct cmd {
	char*			name;
	Tcl_ObjCmdProc*	proc;
	Tcl_ObjCmdProc*	nrproc;
} cmds[] = {
	{NS "::push",				push_cmd,				NULL},
	{NS "::openssl_version",	openssl_version_cmd,	NULL},
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
		if (-1 == s2n_init()) {
			code = TCL_ERROR;
			Tcl_SetErrorCode(interp, "S2N", s2n_strerror_name(s2n_errno), NULL);
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_init failed: %s", s2n_strerror(s2n_errno, "EN")));
			s2n_errno = S2N_ERR_T_OK;
		} else {
			g_init = 1;
		}
	}
	Tcl_MutexUnlock(&g_init_mutex);
	if (code != TCL_OK) goto finally;

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

	Tcl_MutexLock(&g_intreps_mutex);
	if (g_intreps_init == 0) {
		Tcl_InitHashTable(&g_intreps, TCL_ONE_WORD_KEYS);
		g_intreps_init = 1;
	}
	Tcl_MutexUnlock(&g_intreps_mutex);

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
		Tcl_MutexLock(&g_intreps_mutex);
		if (g_intreps_init) {
			Tcl_HashEntry*	he;
			Tcl_HashSearch	search;
			while ((he = Tcl_FirstHashEntry(&g_intreps, &search))) {
				Tcl_Obj*	obj = (Tcl_Obj*)Tcl_GetHashValue(he);
				Tcl_GetString(obj);
				Tcl_FreeInternalRep(obj);	// Calls Tcl_DeleteHashEntry on this entry
			}
			Tcl_DeleteHashTable(&g_intreps);
			g_intreps_init = 0;
		}
		Tcl_MutexUnlock(&g_intreps_mutex);
		Tcl_MutexFinalize(&g_intreps_mutex);
		g_intreps_mutex = NULL;

		Tcl_MutexLock(&g_init_mutex);
		if (g_init) {
			if (-1 == s2n_cleanup())
				Tcl_Panic("s2n_cleanup failed: %s\n", s2n_strerror(s2n_errno, "EN"));
			g_init = 0;
		}
		Tcl_MutexUnlock(&g_init_mutex);
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
