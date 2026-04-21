#include "s2nInt.h"
#include "defer.h"

// Must be kept in sync with the enum in s2nInt.tcl
static const char* lit_str[L_size] = {
	"",				// L_EMPTY
	"1",			// L_TRUE
	"0",			// L_FALSE
};

int g_unloading = 0;

TCL_DECLARE_MUTEX(g_init_mutex)
static int				g_init = 0;
static Tcl_HashTable	g_managed_chans;

TCL_DECLARE_MUTEX(g_intreps_mutex)
static Tcl_HashTable	g_intreps;
static int				g_intreps_init = 0;

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
static const char* action_str(int action) //<<<
{
	switch (action) {
		case TCL_CHANNEL_THREAD_INSERT:		return "TCL_CHANNEL_THREAD_INSERT";
		case TCL_CHANNEL_THREAD_REMOVE:		return "TCL_CHANNEL_THREAD_REMOVE";
		default:							return "<unknown>";
	}
}

//>>>

// Common driver parts <<<
static int s2n_common_chan_input(ClientData cdata, char* buf, int toRead, int* errorCodePtr);
static int s2n_common_chan_output(ClientData cdata, const char* buf, int toWrite, int* errorCodePtr);
static int s2n_common_chan_close2(ClientData cdata, Tcl_Interp* interp, int flags);
#if TCL_MAJOR_VERSION < 9
static int s2n_common_chan_seek(ClientData cdata, long offset, int mode, int* errorCodePtr);
#endif
static Tcl_WideInt s2n_common_chan_wide_seek(ClientData cdata, Tcl_WideInt offset, int mode, int* errorCodePtr);
static void s2n_common_chan_thread_action(ClientData cdata, int action);
// Common driver parts >>>
// Stacked channel implementation <<<
static int s2n_stacked_chan_block_mode(ClientData cdata, int mode);
static int s2n_stacked_chan_set_option(ClientData cdata, Tcl_Interp* interp, const char* optname, const char* optval);
static int s2n_stacked_chan_get_option(ClientData cdata, Tcl_Interp* interp, const char* optname, Tcl_DString* dsPtr);
static void s2n_stacked_chan_watch(ClientData cdata, int mask);
static int s2n_stacked_chan_handler(ClientData cdata, int mask);

Tcl_ChannelType	s2n_stacked_channel_type = {
	.typeName			= "s2n_stacked",
	.version			= TCL_CHANNEL_VERSION_5,
	.blockModeProc		= s2n_stacked_chan_block_mode,
	.close2Proc			= s2n_common_chan_close2,
	.inputProc			= s2n_common_chan_input,
	.outputProc			= s2n_common_chan_output,
	.setOptionProc		= s2n_stacked_chan_set_option,
	.getOptionProc		= s2n_stacked_chan_get_option,
	.watchProc			= s2n_stacked_chan_watch,
	.handlerProc		= s2n_stacked_chan_handler,
	.threadActionProc	= s2n_common_chan_thread_action,
#if TCL_MAJOR_VERSION < 9
	// Tcl 8.6 panics if wideSeekProc is set without seekProc, even though
	// it prefers wideSeekProc when both exist.  Tcl 9 ignores seekProc.
	.seekProc			= s2n_common_chan_seek,
#endif
	.wideSeekProc		= s2n_common_chan_wide_seek,
};

static int s2n_stacked_chan_block_mode(ClientData cdata, int mode) //<<<
{
	struct con_cx*	con_cx = cdata;
	Tcl_DriverBlockModeProc*	base_blockmode = Tcl_ChannelBlockModeProc(Tcl_GetChannelType(con_cx->basechan));
	return base_blockmode(Tcl_GetChannelInstanceData(con_cx->basechan), mode);
}

//>>>
static int s2n_stacked_chan_set_option(ClientData cdata, Tcl_Interp* interp, const char* optname, const char* optval) //<<<
{
	int				code = TCL_OK;
	struct con_cx*	con_cx = cdata;

	if (strcmp(optname, "-servername") == 0) {
		CHECK_S2N_LABEL(finally, code, s2n_set_server_name(con_cx->s2n_con, optval));
	} else {
		code = Tcl_BadChannelOption(interp, optname, "servername");
		Tcl_SetErrno(EINVAL);
		goto finally;
	}

finally:
	return code;
}

//>>>
static int s2n_stacked_chan_get_option(ClientData cdata, Tcl_Interp* interp, const char* optname, Tcl_DString* val) //<<<
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
static void s2n_stacked_chan_watch(ClientData cdata, int mask) //<<<
{
	struct con_cx*	con_cx = cdata;
	const int gotmask = mask;

	if (!con_cx->handshake_done) {
		// While the handshake is busy, signal that we want to be notified when IO is possible
		mask &= TCL_EXCEPTION;
		switch (con_cx->blocked) {
			case S2N_BLOCKED_ON_READ:	mask |= TCL_READABLE; break;
			case S2N_BLOCKED_ON_WRITE:	mask |= TCL_WRITABLE; break;
			default: break;
		}
		CLOGS(HANDSHAKE, "handshake not done, forwarding mask: %s", mask_str(mask));
	}

	CLOGS(WATCH, "gotmask %s, forwarding %s", mask_str(gotmask), mask_str(mask));
	Tcl_DriverWatchProc*	base_watch = Tcl_ChannelWatchProc(Tcl_GetChannelType(con_cx->basechan));
	return base_watch(Tcl_GetChannelInstanceData(con_cx->basechan), mask);
}

//>>>
static int s2n_stacked_chan_handler(ClientData cdata, int mask) //<<<
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
					fprintf(stderr, "s2n_stacked_chan_handler: s2n_negotiate failed: %s, errno: %d\n", s2n_strerror(s2n_errno, "EN"), errno);
					break;

				default:
					fprintf(stderr, "s2n_stacked_chan_handler: s2n_negotiate failed: %s\n", s2n_strerror(s2n_errno, "EN"));
					break;
			}
		}
	}

	CLOGS(IO, "returning %s", mask_str(mask));
	return mask;
}

//>>>
// Stacked channel implementation >>>
// Direct channel implementation <<<
static int s2n_direct_chan_block_mode(ClientData cdata, int mode);
static int s2n_direct_chan_set_option(ClientData cdata, Tcl_Interp* interp, const char* optname, const char* optval);
static int s2n_direct_chan_get_option(ClientData cdata, Tcl_Interp* interp, const char* optname, Tcl_DString* dsPtr);
static void s2n_direct_chan_watch(ClientData cdata, int mask);
static void s2n_direct_chan_handler(ClientData cdata, int mask);

Tcl_ChannelType	s2n_direct_channel_type = {
	.typeName			= "s2n_direct",
	.version			= TCL_CHANNEL_VERSION_5,
	.blockModeProc		= s2n_direct_chan_block_mode,
	.close2Proc			= s2n_common_chan_close2,
	.inputProc			= s2n_common_chan_input,
	.outputProc			= s2n_common_chan_output,
	.setOptionProc		= s2n_direct_chan_set_option,
	.getOptionProc		= s2n_direct_chan_get_option,
	.watchProc			= s2n_direct_chan_watch,
	//.handlerProc		= s2n_direct_chan_handler,		// Called by Tcl_CreateFileHandler
	.threadActionProc	= s2n_common_chan_thread_action,
#if TCL_MAJOR_VERSION < 9
	.seekProc			= s2n_common_chan_seek,
#endif
	.wideSeekProc		= s2n_common_chan_wide_seek,
};

struct s2n_direct_ev {
	Tcl_Event		ev;
	struct con_cx*	con_cx;
	int				mask;
};

static int s2n_direct_chan_block_mode(ClientData cdata, int mode) //<<<
{
	struct con_cx*	con_cx = cdata;

	switch (mode) {
		case TCL_MODE_BLOCKING:
			con_cx->blocking = 1;
			if (-1 == fcntl(con_cx->fd, F_SETFL, fcntl(con_cx->fd, F_GETFL) & ~O_NONBLOCK)) return errno;
			break;
		case TCL_MODE_NONBLOCKING:
			con_cx->blocking = 0;
			if (-1 == fcntl(con_cx->fd, F_SETFL, fcntl(con_cx->fd, F_GETFL) | O_NONBLOCK)) return errno;
			break;
		default:
			return EINVAL;
	}

	CLOGS(IO, "Set block mode: %d", fcntl(con_cx->fd, F_GETFL) & O_NONBLOCK ? 1 : 0);

	return 0;
}

//>>>
static int s2n_direct_chan_set_option(ClientData cdata, Tcl_Interp* interp, const char* optname, const char* optval) //<<<
{
	int				code = TCL_OK;
	struct con_cx*	con_cx = cdata;

	if (strcmp(optname, "-servername") == 0) {
		CHECK_S2N_LABEL(finally, code, s2n_set_server_name(con_cx->s2n_con, optval));
	} else {
		code = Tcl_BadChannelOption(interp, optname, "servername");
		Tcl_SetErrno(EINVAL);
		goto finally;
	}

finally:
	return code;
}

//>>>
static int s2n_direct_chan_get_option(ClientData cdata, Tcl_Interp* interp, const char* optname, Tcl_DString* val) //<<<
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
static void s2n_direct_chan_watch(ClientData cdata, int mask) //<<<
{
	struct con_cx*	con_cx = cdata;
	const int gotmask = mask;

	if (!con_cx->handshake_done) {
		// While the handshake is busy, signal that we want to be notified when IO is possible
		mask &= TCL_EXCEPTION;
		if (!con_cx->connected) mask |= TCL_WRITABLE;
		switch (con_cx->blocked) {
			case S2N_BLOCKED_ON_READ:	mask |= TCL_READABLE; break;
			case S2N_BLOCKED_ON_WRITE:	mask |= TCL_WRITABLE; break;
			default: break;
		}
		CLOGS(HANDSHAKE, "handshake not done, forwarding mask: %s", mask_str(mask));
	}

	CLOGS(WATCH, "gotmask %s, forwarding %s", mask_str(gotmask), mask_str(mask));
	Tcl_CreateFileHandler(con_cx->fd, mask, s2n_direct_chan_handler, con_cx);
}

//>>>
static int s2n_direct_chan_notify(Tcl_Event* ev, int flags) //<<<
{
	(void)flags;
	struct s2n_direct_ev*	s2n_ev = (struct s2n_direct_ev*)ev;
	struct con_cx*			con_cx = s2n_ev->con_cx;
	int						mask = s2n_ev->mask;

	CLOGS(IO, "mask: %s", mask_str(mask));
	Tcl_NotifyChannel(con_cx->chan, mask);
	return 1;	// Event is freed by Tcl
}

//>>>
static void s2n_direct_chan_handler(ClientData cdata, int mask) //<<<
{
	struct con_cx*	con_cx = cdata;

	if (con_cx->connected) {
		CLOGS(IO, "mask: %s, connected: %d, handshake_done: %d", mask_str(mask), con_cx->connected, con_cx->handshake_done);
	}

	if (!con_cx->handshake_done) {
		// While the handshake is busy, don't report readable and writable to
		// the users of this channel - the only IO that can happen on the
		// channel is the TLS handshake, handled internally.
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
						Tcl_CreateFileHandler(con_cx->fd, internal_mask, s2n_direct_chan_handler, con_cx);
					}
					break;
				}

				case S2N_ERR_T_IO:
					fprintf(stderr, "s2n_direct_chan_handler: s2n_negotiate failed: %s, errno: %d\n", s2n_strerror(s2n_errno, "EN"), errno);
					break;

				default:
					fprintf(stderr, "s2n_direct_chan_handler: s2n_negotiate failed: %s\n", s2n_strerror(s2n_errno, "EN"));
					break;
			}
		}
	}

	if (mask) {
		struct s2n_direct_ev*	ev = ckalloc(sizeof(struct s2n_direct_ev));
		*ev = (struct s2n_direct_ev){
			.ev.proc	= s2n_direct_chan_notify,
			.con_cx		= con_cx,
			.mask		= mask,
		};
		CLOGS(IO, "queuing notify event %s", mask_str(mask));
		Tcl_QueueEvent(&ev->ev, TCL_QUEUE_TAIL);
	}
}

//>>>

// Direct channel implementation >>>
// Common parts implementation <<<
static int s2n_common_chan_input(ClientData cdata, char* buf, int toRead, int* errorCodePtr) //<<<
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
			con_cx->read_count += got;
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
					fprintf(stderr, "\ts2n_common_chan_input: s2n_recv failed: %s, errno: %d\n", s2n_strerror(s2n_errno, "EN"), errno);
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
static int s2n_common_chan_output(ClientData cdata, const char* buf, int toWrite, int* errorCodePtr) //<<<
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
	if (g_unloading) {
		CLOGS(IO, "unloading, returning ENOTCONN");
		*errorCodePtr = ENOTCONN;
		bytes_written = -1;
		goto done;
	}

	if (con_cx->handshake_done) {
		while (remain) {
			const int	wrote = s2n_send(con_cx->s2n_con, buf+bytes_written, remain, &blocked);
			CLOGS(IO, "\ts2n_send(%d) wrote %d bytes", remain, wrote);
			if (wrote >= 0) {
				bytes_written += wrote;
				remain -= wrote;
				con_cx->write_count += wrote;
			} else {
				switch (s2n_error_get_type(s2n_errno)) {
					case S2N_ERR_T_BLOCKED:
						if (blocked == S2N_BLOCKED_ON_WRITE) {
							if (bytes_written == 0) {
								*errorCodePtr = EAGAIN;
								bytes_written = -1;
							}
						}
						goto done;

					case S2N_ERR_T_IO:
						CLOGS(IO, "s2n_send error:%s  %s", Tcl_ErrnoId(), Tcl_ErrnoMsg(errno));
						*errorCodePtr = errno;
						bytes_written = -1;
						goto done;

					default:
						CLOGS(IO, "s2n_send error: %s", s2n_strerror(s2n_errno, "EN"));
						*errorCodePtr = EIO;
						bytes_written = -1;
						goto done;
				}
			}
		}
	} else {
		CLOGS(IO, "handshake not done, returning EAGAIN");
		*errorCodePtr = EAGAIN;
		bytes_written = -1;
	}

done:
	CLOGS(IO, "<-- toWrite: %d returning %d", toWrite, bytes_written);
	return bytes_written;
}

//>>>
static int s2n_common_chan_close2(ClientData cdata, Tcl_Interp* interp, int flags) //<<<
{
	struct con_cx* con_cx = cdata;
	CLOGS(IO, "--> %x", flags);

	if (flags & TCL_CLOSE_READ) {
		if (interp) {
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_common_chan_close2: Cannot close read side"));
			Tcl_SetErrorCode(interp, "S2N", "CHAN", "CLOSE2", NULL);
		}
		return EINVAL;
	}

	if (flags & TCL_CLOSE_WRITE) {
		if (con_cx->write_closed) return 0;

		s2n_blocked_status blocked = S2N_NOT_BLOCKED;
		const int rc = s2n_shutdown_send(con_cx->s2n_con, &blocked);
		if (rc == S2N_SUCCESS) {
			con_cx->write_closed = 1;
			if (con_cx->type == CHANTYPE_DIRECT && -1 == shutdown(con_cx->fd, SHUT_WR)) {
				const int ec = errno;
				if (interp) {
					Tcl_SetErrno(ec);
					Tcl_SetErrorCode(interp, "POSIX", Tcl_ErrnoId(), Tcl_ErrnoMsg(ec), NULL);
					Tcl_SetObjResult(interp, Tcl_ObjPrintf("close: %s %s", Tcl_ErrnoId(), Tcl_ErrnoMsg(ec)));
				}
				return ec;
			}
			return 0;
		}

		const int err_type = s2n_error_get_type(s2n_errno);
		if (err_type == S2N_ERR_T_BLOCKED || err_type == S2N_ERR_T_CLOSED) {
			con_cx->write_closed = 1;
			return 0;
		}
		if (interp) {
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_shutdown_send failed: %s", s2n_strerror(s2n_errno, "EN")));
			Tcl_SetErrorCode(interp, "S2N", "CHAN", "CLOSE2", NULL);
		}
		switch (err_type) {
			case S2N_ERR_T_IO:		return errno ? errno : EIO;
			case S2N_ERR_T_PROTO:	return EPROTO;
			default:				return EINVAL;
		}
	}

	if (flags != 0) {
		if (interp) {
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_common_chan_close2: unknown flags: %d", flags));
			Tcl_SetErrorCode(interp, "S2N", "CHAN", "CLOSE2", NULL);
		}
		return EINVAL;
	}

	// flags == 0: full close.
	CLOGS(LIFECYCLE, "closing connection %s", S2N_CON_NAME(con_cx->s2n_con));

	int posixcode = 0;

	// The fd + con_cx tear-down always runs, even if s2n_shutdown fails —
	// leaving the Tcl channel alive with a typePtr into our library's data
	// segment would crash TclFinalizeIOSubsystem after library unload.
	// Reporting a close() errno is suppressed when we've already recorded a
	// more informative s2n_shutdown error.
	defer {
		const int is_direct = con_cx->type == CHANTYPE_DIRECT;
		int rc = 0;
		if (is_direct) {
			// Remove Tcl's internal file handler before closing the fd;
			// without this Tcl's notifier leaks its handler struct (fd
			// lookup happens by integer, and a closed fd can't be matched).
			Tcl_DeleteFileHandler(con_cx->fd);
			rc = close(con_cx->fd);
		}
		free_con_cx(con_cx);
		if (is_direct && rc == -1 && posixcode == 0) {
			const int ec = errno;
			posixcode = ec;
			if (interp) {
				Tcl_SetErrno(ec);
				Tcl_SetErrorCode(interp, "POSIX", Tcl_ErrnoId(), Tcl_ErrnoMsg(ec), NULL);
				Tcl_SetObjResult(interp, Tcl_ObjPrintf("close: %s %s", Tcl_ErrnoId(), Tcl_ErrnoMsg(ec)));
			}
		}
		CLOGS(IO, "<-- 0 returning %d", posixcode);
	}

	if (!con_cx->write_closed) {
		s2n_blocked_status blocked = S2N_NOT_BLOCKED;
		CLOGS(IO, "calling s2n_shutdown %s", S2N_CON_NAME(con_cx->s2n_con));
		const int rc = s2n_shutdown(con_cx->s2n_con, &blocked);
		// TODO: Handle blocked?
		if (rc != S2N_SUCCESS) {
			const int err_type = s2n_error_get_type(s2n_errno);
			// BLOCKED / CLOSED are non-fatal — peer already gone, or handshake
			// never completed. Record other errors on the interp but let the
			// defer tear things down regardless.
			if (err_type != S2N_ERR_T_BLOCKED && err_type != S2N_ERR_T_CLOSED) {
				if (interp) {
					Tcl_SetObjResult(interp, Tcl_ObjPrintf("s2n_shutdown failed: %s", s2n_strerror(s2n_errno, "EN")));
					Tcl_SetErrorCode(interp, "S2N", "CHAN", "CLOSE2", NULL);
				}
				switch (err_type) {
					case S2N_ERR_T_IO:		posixcode = errno ? errno : EIO;	break;
					case S2N_ERR_T_PROTO:	posixcode = EPROTO;					break;
					default:				posixcode = EINVAL;					break;
				}
			}
		}
	}

	return posixcode;
}

//>>>
static void s2n_common_chan_thread_action(ClientData cdata, int action) //<<<
{
	struct con_cx*	con_cx = cdata;
	CLOGS(LIFECYCLE, "%s: %s", S2N_CON_NAME(con_cx->s2n_con), action_str(action));
}

//>>>
#if TCL_MAJOR_VERSION < 9
static int s2n_common_chan_seek(ClientData cdata, long offset, int mode, int* errorCodePtr) //<<<
{
	// Stub: Tcl 8.6 prefers wideSeekProc but panics if seekProc is NULL.
	(void)cdata; (void)offset; (void)mode;
	*errorCodePtr = EINVAL;
	return -1;
}

//>>>
#endif
static Tcl_WideInt s2n_common_chan_wide_seek(ClientData cdata, Tcl_WideInt offset, int mode, int* errorCodePtr) //<<<
{
	struct con_cx*	con_cx = cdata;

	CLOGS(IO, "--> offset: %" TCL_LL_MODIFIER "d, mode: %d", (long long)offset, mode);
	if (mode == SEEK_CUR) {
		return (Tcl_WideInt)con_cx->write_count;
	}
	*errorCodePtr = EINVAL;
	return -1;
}

//>>>

// Common parts implementation >>>

// Internal API <<<
void free_interp_cx(ClientData cdata, Tcl_Interp* interp) //<<<
{
	(void)interp;
	struct interp_cx*	l = (struct interp_cx*)cdata;

	CLOGS(LIFECYCLE, "free_interp_cx");
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
static void register_chan(struct con_cx* con_cx) //<<<
{
	Tcl_HashEntry*		he = NULL;
	int					new = 0;

	CLOGS(LIFECYCLE, "registering chan %s", clogs_name(con_cx));
	Tcl_MutexLock(&g_init_mutex);
	he = Tcl_CreateHashEntry(&g_managed_chans, con_cx, &new);
	if (!new) Tcl_Panic("register_chan: already registered");
	Tcl_SetHashValue(he, con_cx);
	con_cx->registered = 1;
	Tcl_MutexUnlock(&g_init_mutex);
}

//>>>
static void forget_chan(struct con_cx* con_cx) //<<<
{
	Tcl_HashEntry*		he = NULL;

	CLOGS(LIFECYCLE, "forgetting chan %s", clogs_name(con_cx));
	Tcl_MutexLock(&g_init_mutex);
	he = Tcl_FindHashEntry(&g_managed_chans, con_cx);
	if (!he) Tcl_Panic("forget_chan: not registered");
	Tcl_DeleteHashEntry(he);
	Tcl_MutexUnlock(&g_init_mutex);
}

//>>>

static void free_s2n_config_intrep(Tcl_Obj* obj);
static void dup_s2n_config_intrep(Tcl_Obj* src, Tcl_Obj* dst);

Tcl_ObjType s2n_config_type = {
	.name			= "::s2n::config",
	.freeIntRepProc	= free_s2n_config_intrep,
	.dupIntRepProc	= dup_s2n_config_intrep,
};

// Drop one reference on an s2n_config_rc. When the last reference goes,
// s2n_config_free the underlying C struct and release the wrapper.
static void tcls2n_config_rc_release(struct s2n_config_rc** rc) //<<<
{
	if (*rc == NULL) return;
	if (--(*rc)->refCount == 0) {
		if ((*rc)->c) {
			if (-1 == s2n_config_free((*rc)->c))
				CLOGS(LIFECYCLE, "s2n_config_free failed: %s\n", s2n_strerror(s2n_errno, "EN"));
			(*rc)->c = NULL;
		}
		ckfree(*rc);
		*rc = NULL;
	}
}

//>>>
static void free_s2n_config_intrep(Tcl_Obj* obj) //<<<
{
	CLOGS(LIFECYCLE, "freeing config intrep %s", clogs_name(obj));
	forget_intrep(obj);
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &s2n_config_type);
	if (ir) tcls2n_config_rc_release((struct s2n_config_rc**)&ir->twoPtrValue.ptr1);
}

//>>>
static void dup_s2n_config_intrep(Tcl_Obj* src, Tcl_Obj* dst) //<<<
{
	Tcl_Size	len;
	const char*	str = Tcl_GetStringFromObj(src, &len);
	Tcl_InitStringRep(dst, str, len);
}

//>>>
// Build or recover an s2n_config_rc from a Tcl_Obj. The returned rc is NOT
// ref-bumped by this call; callers that need to retain it past the life of
// the Tcl_Obj (e.g. past a shimmer) must increment refCount themselves.
static int get_s2n_config_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct s2n_config_rc** config_rc) //<<<
{
	Tcl_ObjInternalRep* ir = Tcl_FetchInternalRep(obj, &s2n_config_type);
	if (ir) {
		*config_rc = (struct s2n_config_rc*)ir->twoPtrValue.ptr1;
		return TCL_OK;
	}

	struct s2n_config_rc* rc = (struct s2n_config_rc*)ckalloc(sizeof *rc);
	*rc = (struct s2n_config_rc){.refCount = 1};	// build-time ref, handed off to
													// the intrep on success; released
													// by the defer below on any failure
	rc->c = s2n_config_new();
	defer { tcls2n_config_rc_release(&rc); }

	struct s2n_config*	c = rc->c;
	Tcl_Obj*			key = NULL;
	Tcl_Obj*			val = NULL;
	int					done;
	const char*			ca_file = NULL;
	const char*			ca_dir  = NULL;

	// The dict walk: no need for a search_active flag — the defer that calls
	// Tcl_DictObjDone is only registered if Tcl_DictObjFirst succeeds.
	// (Portable across Tcl 9 and 8.6, whose "no active search" sentinel values
	// for Tcl_DictSearch.epoch disagree: 0 vs -1.)
	Tcl_DictSearch search;
	TEST_OK(Tcl_DictObjFirst(interp, obj, &search, &key, &val, &done));
	defer { Tcl_DictObjDone(&search); }

	for (; !done; Tcl_DictObjNext(&search, &key, &val, &done)) {
		static const char* config_names[] = {
			"session_tickets",
			"ticket_lifetime",
			"cipher_preferences",
			"ca_file",
			"ca_dir",
			NULL
		};
		enum config {
			CONFIG_SESSION_TICKETS,
			CONFIG_TICKET_LIFETIME,
			CONFIG_CIPHER_PREFERENCES,
			CONFIG_CA_FILE,
			CONFIG_CA_DIR,
		} conf_name;
		int conf_name_int;

		TEST_OK(Tcl_GetIndexFromObj(interp, key, config_names, "config", TCL_EXACT, &conf_name_int));
		conf_name = conf_name_int;

		switch (conf_name) {
			case CONFIG_SESSION_TICKETS:
			{
				int enabled;
				TEST_OK(Tcl_GetBooleanFromObj(interp, val, &enabled));
				CHECK_S2N(s2n_config_set_session_cache_onoff(c, enabled));
				break;
			}

			case CONFIG_TICKET_LIFETIME:
			{
				Tcl_Obj**	ov;
				Tcl_Size	oc;
				Tcl_WideInt	lifetime;

				TEST_OK(Tcl_ListObjGetElements(interp, val, &oc, &ov));
				if (oc != 2) THROW_ERROR("ticket_lifetime must be a list of two integers: ", Tcl_GetString(val));
				TEST_OK(Tcl_GetWideIntFromObj(interp, ov[0], &lifetime));
				CHECK_S2N(s2n_config_set_ticket_encrypt_decrypt_key_lifetime(c, lifetime));
				TEST_OK(Tcl_GetWideIntFromObj(interp, ov[1], &lifetime));
				CHECK_S2N(s2n_config_set_ticket_decrypt_key_lifetime(c, lifetime));
				break;
			}

			case CONFIG_CIPHER_PREFERENCES:
				CHECK_S2N(s2n_config_set_cipher_preferences(c, Tcl_GetString(val)));
				break;

			case CONFIG_CA_FILE:
				ca_file = Tcl_GetString(val);
				break;

			case CONFIG_CA_DIR:
				ca_dir = Tcl_GetString(val);
				break;

			default: THROW_ERROR("Unhandled config: ", Tcl_GetString(key));
		}
	}

	// Apply buffered ca_file / ca_dir as a single s2n call — the API accepts both at
	// once (either may be NULL) and a caller dict that sets only ca_dir must not
	// clobber an earlier ca_file (and vice-versa). Wipe the default trust store first
	// so the supplied bundle *replaces* system roots — matches AWS CLI AWS_CA_BUNDLE
	// semantics. See s2n.h:850-868 for the underlying API's additive default.
	if (ca_file != NULL || ca_dir != NULL) {
		CHECK_S2N(s2n_config_wipe_trust_store(c));
		CHECK_S2N(s2n_config_set_verification_ca_location(c, ca_file, ca_dir));
	}

	Tcl_GetString(obj);	// Ensure that the string rep is generated before we take over the intrep - we can't generate our own
	Tcl_StoreInternalRep(obj, &s2n_config_type, &(Tcl_ObjInternalRep){.twoPtrValue.ptr1 = rc});
	register_intrep(obj);
	CLOGS(LIFECYCLE, "created config %s", clogs_name(obj));

	*config_rc = rc;
	rc = NULL;	// handoff: intrep now owns the build-time ref. Null the local
				// so the defer's release becomes a no-op.
	return TCL_OK;
}

//>>>
void free_con_cx(struct con_cx* con_cx) //<<<
{
	CLOGS(LIFECYCLE, "free_con_cx: %s", clogs_name(con_cx));
	if (con_cx->registered) forget_chan(con_cx);
	if (con_cx->s2n_con) {
		CLOGS(LIFECYCLE, "Freeing s2n connection: %s", S2N_CON_NAME(con_cx->s2n_con));
		if (-1 == s2n_connection_free(con_cx->s2n_con)) {
			Tcl_Panic("s2n_connection_free failed: %s\n", s2n_strerror(s2n_errno, "EN"));
		}
		con_cx->s2n_con = NULL;
	}
	tcls2n_config_rc_release(&con_cx->config);
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
static OBJCMD(push_cmd) //<<<
{
	(void)cdata;
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
	int roleint;

	enum {A_cmd, A_CHAN, A_args};
	CHECK_MIN_ARGS("channelName ?-opt val ...?");

	int basemode;
	Tcl_Channel basechan = Tcl_GetChannel(interp, Tcl_GetString(objv[A_CHAN]), &basemode);
	if ((basemode & TCL_READABLE) == 0 || (basemode & TCL_WRITABLE) == 0)
		THROW_ERROR("Channel must be readable and writable");

	struct con_cx* con_cx = (struct con_cx*)ckalloc(sizeof *con_cx);
	*con_cx = (struct con_cx){
		.type		= CHANTYPE_STACKED,
		.basechan	= basechan,
		.blocked	= S2N_NOT_BLOCKED,
	};
	CLOGS(LIFECYCLE, "Created con_cx: %s", clogs_name(con_cx));
	defer { if (con_cx) free_con_cx(con_cx); }

	// Pre-scan for -role so s2n_connection_new can see it.
	for (int i=A_args; i<objc; i++) {
		int optint;
		TEST_OK(Tcl_GetIndexFromObj(interp, objv[i], opts, "option", 0, &optint));
		const enum opt o = optint;
		switch (o) {
			case OPT_ROLE:
				TEST_OK(Tcl_GetIndexFromObj(interp, objv[++i], s2n_role_str, "role", 0, &roleint));
				role = roleint;
				break;

			case OPT_CONFIG:
			case OPT_SERVERNAME:
			case OPT_PREFER:
				i++;
				break;

			default:
				THROW_ERROR("Unhandled option: ", Tcl_GetString(objv[i]));
		}
	}

	con_cx->s2n_con = s2n_connection_new(role == ROLE_CLIENT ? S2N_CLIENT : S2N_SERVER);
	CLOGS(LIFECYCLE, "Created s2n connection: %s", S2N_CON_NAME(con_cx->s2n_con));

	for (int i=A_args; i<objc; i++) {
		int optint;
		TEST_OK(Tcl_GetIndexFromObj(interp, objv[i], opts, "option", 0, &optint));
		const enum opt o = optint;
		switch (o) {
			case OPT_ROLE:
				i++;
				break;

			case OPT_CONFIG: //<<<
			{
				struct s2n_config_rc* config_rc = NULL;
				if (i == objc-1) THROW_ERROR("Missing value for -config");
				Tcl_Obj* cfg_obj = objv[++i];
				TEST_OK(get_s2n_config_from_obj(interp, cfg_obj, &config_rc));
				// Retain the config at the C refcount layer — independent of
				// the cfg_obj Tcl_Obj. If cfg_obj shimmers to another type
				// (dict, list, string) later, its intrep drops its ref but
				// ours keeps the struct s2n_config alive for as long as the
				// connection lives.
				tcls2n_config_rc_release(&con_cx->config);
				con_cx->config = config_rc;
				con_cx->config->refCount++;
				CHECK_S2N(s2n_connection_set_config(con_cx->s2n_con, config_rc->c));
				break;
			}
			//>>>

			case OPT_SERVERNAME:
				if (i == objc-1) THROW_ERROR("Missing value for -servername");
				CHECK_S2N(s2n_set_server_name(con_cx->s2n_con, Tcl_GetString(objv[++i])));
				break;

			case OPT_PREFER: //<<<
			{
				static const char* s2n_prefer_str[] = { "throughput", "latency", NULL };
				enum prefer { PREFER_THROUGHPUT, PREFER_LATENCY } prefer = PREFER_THROUGHPUT;
				int preferint;

				if (i == objc-1) THROW_ERROR("Missing value for -prefer");
				TEST_OK(Tcl_GetIndexFromObj(interp, objv[++i], s2n_prefer_str, "prefer", 0, &preferint));
				prefer = preferint;
				switch (prefer) {
					case PREFER_THROUGHPUT:	CHECK_S2N(s2n_connection_prefer_throughput(con_cx->s2n_con));	break;
					case PREFER_LATENCY:	CHECK_S2N(s2n_connection_prefer_low_latency(con_cx->s2n_con));	break;
					default:				THROW_ERROR("Unhandled prefer: ", Tcl_GetString(objv[i]));
				}
				break;
			}
			//>>>

			default:
				THROW_ERROR("Unhandled option: ", Tcl_GetString(objv[i]));
		}
	}

	// Wire up IO callbacks to read and write to the base chan
	CHECK_S2N(s2n_connection_set_send_ctx(con_cx->s2n_con, con_cx));
	CHECK_S2N(s2n_connection_set_recv_ctx(con_cx->s2n_con, con_cx));
	CHECK_S2N(s2n_connection_set_send_cb(con_cx->s2n_con, s2n_basechan_send));
	CHECK_S2N(s2n_connection_set_recv_cb(con_cx->s2n_con, s2n_basechan_recv));

	CLOGS(HANDSHAKE, "s2n_negotiate");
	const int neg_rc = s2n_negotiate(con_cx->s2n_con, &con_cx->blocked);

	if (neg_rc == S2N_SUCCESS) {
		CLOGS(HANDSHAKE, "s2n_negotiate success");
		con_cx->handshake_done = 1;
	} else {
		CLOGS(HANDSHAKE, "s2n_strerror_name: %s: %s", s2n_strerror_name(s2n_errno), s2n_strerror(s2n_errno, "EN"));
		switch (s2n_error_get_type(s2n_errno)) {
			case S2N_ERR_T_BLOCKED:
			{
				int mask = 0;
				switch (con_cx->blocked) {
					case S2N_BLOCKED_ON_READ:	mask |= TCL_READABLE;	break;
					case S2N_BLOCKED_ON_WRITE:	mask |= TCL_WRITABLE;	break;
					default:	break;
				}
				if (mask) {
					CLOGS(HANDSHAKE, "s2n_negotiate blocked on %s, registering watch for %s",
							con_cx->blocked == S2N_BLOCKED_ON_READ ? "read" : "write", mask_str(mask));
					Tcl_DriverWatchProc* base_watch = Tcl_ChannelWatchProc(Tcl_GetChannelType(con_cx->basechan));
					base_watch(Tcl_GetChannelInstanceData(con_cx->basechan), mask);
				}
				break;
			}

			default:
				THROW_ERROR("s2n_negotiate failed: ", s2n_strerror(s2n_errno, "EN"));
		}
	}

	con_cx->chan = Tcl_StackChannel(interp, &s2n_stacked_channel_type, con_cx,
			TCL_READABLE | TCL_WRITABLE, basechan);
	register_chan(con_cx);
	CLOGS(HANDSHAKE, "leaving push_cmd, handshake_done: %d", con_cx->handshake_done);
	con_cx = NULL;	// handoff to the s2n_stacked_channel_type driver; null so the
					// defer becomes a no-op. Any close2Proc invocation from Tcl
					// later will run free_con_cx via the driver vtable.
	return TCL_OK;
}

//>>>
static OBJCMD(socket_cmd) //<<<
{
	(void)cdata;
	static const char* opts[] = {
		"-async",
		"-config",
		"-servername",
		"-prefer",
		NULL
	};
	enum opt {
		OPT_ASYNC,
		OPT_CONFIG,
		OPT_SERVERNAME,
		OPT_PREFER,
	};
	int					async = 0;
	struct sockaddr_un	uds = {.sun_family = AF_UNIX};
	int					s = -1;	// socket fd (pre-handoff)
	int					connected = 0;

	enum {A_cmd, A_x, A_y, A_args};
	CHECK_MIN_ARGS("?-opt val ...? host port");
	const int A_HOST = objc-2;
	const int A_PORT = objc-1;

	// defer { if (s != -1) close(s); } — raw fd ownership before handoff to con_cx
	defer {
		if (s != -1) {
			if (-1 == close(s)) CLOGS(IO, "close failed: %s", strerror(errno));
		}
	}

	struct con_cx* con_cx = (struct con_cx*)ckalloc(sizeof *con_cx);
	*con_cx = (struct con_cx){
		.type		= CHANTYPE_DIRECT,
		.blocked	= S2N_NOT_BLOCKED,
		.blocking	= 1,
	};
	CLOGS(LIFECYCLE, "Created con_cx: %s", clogs_name(con_cx));
	// defer: if we leave without handing con_cx off to Tcl's channel driver,
	// tear it down. If Tcl_RegisterChannel succeeded, the chan already owns
	// con_cx — we'll Tcl_UnregisterChannel instead so the driver's close2Proc
	// gets called (which will free con_cx via the regular close path).
	Tcl_Channel chan_for_cleanup = NULL;
	defer {
		if (con_cx) {
			if (chan_for_cleanup) {
				Tcl_UnregisterChannel(interp, chan_for_cleanup);
			} else {
				free_con_cx(con_cx);
			}
		}
	}

	con_cx->s2n_con = s2n_connection_new(S2N_CLIENT);
	CLOGS(LIFECYCLE, "Created s2n connection: %s", S2N_CON_NAME(con_cx->s2n_con));

	Tcl_Size	host_len;
	const char*	host = Tcl_GetStringFromObj(objv[A_HOST], &host_len);
	struct addrinfo		static_addr = {0};
	struct addrinfo*	addrs = NULL;
	defer { if (addrs && addrs != &static_addr) freeaddrinfo(addrs); }

	if (host_len == 0) {
		// UDS mode: port is a path
		Tcl_Size	pathlen;
		const char*	path = Tcl_GetStringFromObj(objv[A_PORT], &pathlen);

		if ((size_t)pathlen > sizeof(uds.sun_path)-1)
			THROW_ERROR("Path too long: ", Tcl_GetString(objv[A_PORT]));

		strncpy(uds.sun_path, path, sizeof(uds.sun_path)-1);
		uds.sun_path[sizeof(uds.sun_path)-1] = 0;

		static_addr = (struct addrinfo){
			.ai_family		= AF_UNIX,
			.ai_socktype	= SOCK_STREAM,
			.ai_addr		= (struct sockaddr*)&uds,
			.ai_addrlen		= sizeof(uds),
		};
		addrs = &static_addr;
	} else {
		// TCP mode: port is a port
		const char*	serv = Tcl_GetString(objv[A_PORT]);
		struct addrinfo hints = {
			.ai_family		= AF_UNSPEC,
			.ai_socktype	= SOCK_STREAM,
			.ai_protocol	= IPPROTO_TCP,
		};

		const int rc = getaddrinfo(host, serv, &hints, &addrs);
		if (rc != 0)
			THROW_ERROR("couldn't open socket: ", gai_strerror(rc));

		// If host is a hostname (not a numeric address), use it as the default -servername
		uint8_t ignored[sizeof(struct in6_addr)];
		if (0 == inet_pton(AF_INET, host, ignored) && 0 == inet_pton(AF_INET6, host, ignored))
			CHECK_S2N(s2n_set_server_name(con_cx->s2n_con, host));
	}

	for (int i=A_x; i<A_HOST; i++) {
		int optint;
		TEST_OK(Tcl_GetIndexFromObj(interp, objv[i], opts, "option", 0, &optint));
		const enum opt o = optint;
		switch (o) {
			case OPT_ASYNC:
				async = 1;
				con_cx->blocking = 0;
				break;

			case OPT_CONFIG: //<<<
			{
				struct s2n_config_rc* config_rc = NULL;
				if (i == objc-1) THROW_ERROR("Missing value for -config");
				Tcl_Obj* cfg_obj = objv[++i];
				TEST_OK(get_s2n_config_from_obj(interp, cfg_obj, &config_rc));
				tcls2n_config_rc_release(&con_cx->config);
				con_cx->config = config_rc;
				con_cx->config->refCount++;
				CHECK_S2N(s2n_connection_set_config(con_cx->s2n_con, config_rc->c));
				break;
			}
			//>>>

			case OPT_SERVERNAME:
				if (i == objc-1) THROW_ERROR("Missing value for -servername");
				CHECK_S2N(s2n_set_server_name(con_cx->s2n_con, Tcl_GetString(objv[++i])));
				break;

			case OPT_PREFER: //<<<
			{
				static const char* s2n_prefer_str[] = { "throughput", "latency", NULL };
				enum prefer { PREFER_THROUGHPUT, PREFER_LATENCY } prefer = PREFER_THROUGHPUT;
				int preferint;

				if (i == objc-1) THROW_ERROR("Missing value for -prefer");
				TEST_OK(Tcl_GetIndexFromObj(interp, objv[++i], s2n_prefer_str, "prefer", 0, &preferint));
				prefer = preferint;
				switch (prefer) {
					case PREFER_THROUGHPUT:	CHECK_S2N(s2n_connection_prefer_throughput(con_cx->s2n_con));	break;
					case PREFER_LATENCY:	CHECK_S2N(s2n_connection_prefer_low_latency(con_cx->s2n_con));	break;
					default:				THROW_ERROR("Unhandled prefer: ", Tcl_GetString(objv[i]));
				}
				break;
			}
			//>>>

			default:
				THROW_ERROR("Unhandled option: ", Tcl_GetString(objv[i]));
		}
	}

	for (struct addrinfo* addr=addrs; addr; addr=addr->ai_next) {
		s = socket(addr->ai_family, addr->ai_socktype | (async ? SOCK_NONBLOCK : 0) | SOCK_CLOEXEC, addr->ai_protocol);
		if (s == -1) {
			CLOGS(IO, "socket failed: %s", strerror(errno));
			continue;
		}
		if (-1 == connect(s, addr->ai_addr, addr->ai_addrlen)) {
			if (errno == EINPROGRESS) {
				CLOGS(IO, "connect in progress");
			} else {
				CLOGS(IO, "connect failed: %s", strerror(errno));
				close(s);
				s = -1;
				continue;
			}
		}
		connected = 1;
		break;
	}

	if (!connected) THROW_POSIX("couldn't open socket");

	CLOGS(IO, "setting fd: %d", s);
	con_cx->fd = s;
	s = -1;		// handoff: con_cx owns the fd now; the s-defer becomes a no-op
	CHECK_S2N(s2n_connection_set_fd(con_cx->s2n_con, con_cx->fd));

	con_cx->chan = Tcl_CreateChannel(&s2n_direct_channel_type, clogs_name(con_cx), con_cx, TCL_READABLE | TCL_WRITABLE);
	Tcl_RegisterChannel(interp, con_cx->chan);
	register_chan(con_cx);
	chan_for_cleanup = con_cx->chan;	// signal to the defer: unregister chan
										// on error-exit (which triggers close2Proc
										// → free_con_cx) rather than calling
										// free_con_cx directly.

	if (async) {
		CLOGS(IO, "async mode, registering watch for %s", mask_str(TCL_WRITABLE));
		// TODO: this errors in epoll_ctl with EBADF for -async, investigate the mess: https://cr.yp.to/docs/connect.html
		Tcl_CreateFileHandler(con_cx->fd, TCL_WRITABLE, s2n_direct_chan_handler, con_cx);
	} else {
		con_cx->connected = 1;

		CLOGS(HANDSHAKE, "s2n_negotiate");
		const int neg_rc = s2n_negotiate(con_cx->s2n_con, &con_cx->blocked);

		if (neg_rc == S2N_SUCCESS) {
			CLOGS(HANDSHAKE, "s2n_negotiate success");
			con_cx->handshake_done = 1;
		} else {
			switch (s2n_error_get_type(s2n_errno)) {
				case S2N_ERR_T_BLOCKED:
				{
					int mask = 0;
					switch (con_cx->blocked) {
						case S2N_BLOCKED_ON_READ:	mask |= TCL_READABLE;	break;
						case S2N_BLOCKED_ON_WRITE:	mask |= TCL_WRITABLE;	break;
						default:	break;
					}
					if (mask) {
						CLOGS(HANDSHAKE, "s2n_negotiate blocked on %s, registering watch for %s",
								con_cx->blocked == S2N_BLOCKED_ON_READ ? "read" : "write", mask_str(mask));
						Tcl_CreateFileHandler(con_cx->fd, mask, s2n_direct_chan_handler, con_cx);
					}
					break;
				}

				default:
					THROW_ERROR("s2n_negotiate failed: ", s2n_strerror(s2n_errno, "EN"));
			}
		}
	}

	Tcl_SetObjResult(interp, Tcl_NewStringObj(clogs_name(con_cx), -1));		// TODO: channelName with better guarantees of uniqueness

	CLOGS(HANDSHAKE, "leaving socket_cmd, handshake_done: %d", con_cx->handshake_done);
	con_cx = NULL;		// handoff: null the local so the defer becomes a no-op.
						// The Tcl_Channel already owns con_cx (established via
						// Tcl_CreateChannel above).
	return TCL_OK;
}

//>>>
static OBJCMD(openssl_version_cmd) //<<<
{
	(void)cdata;
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
	{NS "::socket",				socket_cmd,				NULL},
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
	CLOGS(LIFECYCLE, "loading into interp %s", clogs_name(interp));

	Tcl_MutexLock(&g_init_mutex);
	if (!g_init) {
		CLOGS(LIFECYCLE, "calling s2n_init");
		Tcl_InitHashTable(&g_managed_chans, TCL_ONE_WORD_KEYS);
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
const char* unload_flags_str(int flags) //<<<
{
	switch (flags) {
		case TCL_UNLOAD_DETACH_FROM_INTERPRETER:	return "TCL_UNLOAD_DETACH_FROM_INTERPRETER";
		case TCL_UNLOAD_DETACH_FROM_PROCESS:		return "TCL_UNLOAD_DETACH_FROM_PROCESS";
		default:									return "unknown";
	}
}

//>>>
DLLEXPORT int S2n_Unload(Tcl_Interp* interp, int flags) //<<<
{
	int			code = TCL_OK;

	CLOGS(LIFECYCLE, "--> unloading from %s: flags: %s", clogs_name(interp), unload_flags_str(flags));
	Tcl_DeleteAssocData(interp, PACKAGE_NAME);	// Have to do this here, otherwise Tcl will try to call it after we're unloaded

	if (flags == TCL_UNLOAD_DETACH_FROM_PROCESS) {
		// We DELIBERATELY do not clean up abandoned channels on
		// TCL_UNLOAD_DETACH_FROM_INTERPRETER. Doing so would leak fds and
		// memory when child interps with -async s2n::sockets get deleted
		// mid-handshake in a long-running process (see the knownBug test
		// socket-leak-*). A naive per-interp cleanup is dangerous:
		//   - con_cx has no reliable "owning interp" field: chan transfer
		//     moves a channel between interps (common in threaded
		//     acceptor/worker server patterns); thread::attach moves it
		//     between threads. A transferred chan may be intentionally
		//     alive and well in a completely different interp.
		//   - Tcl has no public API to ask "is this channel still referenced
		//     by any interp" — only the now-stale cached pointer we'd have
		//     at chan-create time.
		//   - Closing a perfectly-good chan because its creating interp
		//     happens to be shutting down is a correctness bug that's far
		//     worse than the fd leak.
		// So: we accept the leak for TCL_UNLOAD_DETACH_FROM_INTERPRETER and
		// only force-close at process unload time.

		g_unloading = 1;

		// Order matters: close managed channels first, THEN free intreps,
		// THEN s2n_cleanup_final. Each con_cx->config holds a reference on
		// an s2n_config_rc; closing the channel releases that ref. With the
		// refcount wrapper the ordering is less critical than it was with
		// the Tcl_Obj-linked scheme (freeing the intrep just drops one
		// ref), but closing chans first is still cleaner: it ensures all
		// s2n_connections are gone before s2n_cleanup_final runs.
		//
		// Safety note for Tcl_NotifyChannel below: FlushChannel with
		// calledFromAsyncFlush=1 calls our outputProc synchronously.
		// g_unloading (set just above) short-circuits outputProc to return
		// -1/ENOTCONN immediately — no s2n call, no socket I/O, no retry
		// on EAGAIN. FlushChannel's fatal-error branch (tclIO.c:2904)
		// discards the queue and returns. This is why Tcl_NotifyChannel
		// here can't block. If you reuse this pattern OUTSIDE the global-
		// unload context (i.e. without g_unloading), that guarantee is
		// gone — plan to close con_cx->fd first so s2n_send fails fatally.
		Tcl_MutexLock(&g_init_mutex);
		if (g_init) {
			Tcl_HashEntry*	he;
			Tcl_HashSearch	search;
			CLOGS(LIFECYCLE, "closing managed channels");
			while ((he = Tcl_FirstHashEntry(&g_managed_chans, &search))) {
				struct con_cx*	con_cx = (struct con_cx*)Tcl_GetHashValue(he);
				Tcl_Channel		chan = con_cx->chan;
				if (con_cx->type == CHANTYPE_STACKED) {
					CLOGS(LIFECYCLE, "unstacking stacked channel: %s", clogs_name(con_cx));
					Tcl_UnstackChannel(interp, chan);
				} else {
					// Any direct chan still in our hash at DETACH_FROM_PROCESS
					// time is one that Tcl couldn't close cleanly — e.g. an
					// s2n::socket -async abandoned mid-handshake. The flush
					// during the owner's interp delete returned EAGAIN from
					// our outputProc, Tcl set BG_FLUSH_SCHEDULED, deferred
					// close2(0), and the chan stayed alive in tsdPtr->firstCSPtr.
					//
					// A naive close from here loops: FlushChannel returns 0
					// immediately when BG_FLUSH_SCHEDULED is set (tclIO.c:2786),
					// so CloseChannel — and therefore close2(0) — is never
					// reached and the chan stays in our hash.
					//
					// Tcl_NotifyChannel(chan, TCL_WRITABLE) drives FlushChannel
					// with calledFromAsyncFlush=1 (tclIO.c:8627), which bypasses
					// the early return. With g_unloading set, our outputProc
					// returns -1/ENOTCONN, FlushChannel hits its fatal-error
					// branch (tclIO.c:2904), DiscardOutputQueued's the queued
					// bytes, breaks out of the write loop, and the post-loop
					// block at tclIO.c:2950 ResetFlag's BG_FLUSH_SCHEDULED
					// because the queue is now empty.
					//
					// After that, Tcl_UnregisterChannel goes through normally:
					// refCount is already 0 (decremented by the owner interp's
					// DeleteChannelTable), DetachChannel(NULL, chan) drops it
					// to -1, and then TclClose runs — CHANNEL_CLOSED set,
					// FlushChannel without the early return, CloseChannel
					// preconditions met. Tcl frees the Channel/ChannelState
					// structs while our close2Proc(0) frees con_cx.
					//
					// Per the docs (Tcl_RegisterChannel manpage), a channel
					// that has ever been registered must be closed via
					// Tcl_UnregisterChannel rather than Tcl_Close, so we use
					// the former even though refCount is already 0.
					CLOGS(LIFECYCLE, "draining and closing direct channel: %s", clogs_name(con_cx));
					Tcl_NotifyChannel(chan, TCL_WRITABLE);
					Tcl_UnregisterChannel(NULL, chan);
				}
			}
			Tcl_DeleteHashTable(&g_managed_chans);
		}

		Tcl_MutexLock(&g_intreps_mutex);
		if (g_intreps_init) {
			Tcl_HashEntry*	he;
			Tcl_HashSearch	search;
			CLOGS(LIFECYCLE, "converting intreps to pure strings");
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

		if (g_init) {
			// Explicit unload: run s2n_cleanup_final now so the caller's
			// expected side-effect (the library is fully released) happens
			// synchronously. The atexit handler registered by s2n_init
			// guards with its own "initialized" flag and becomes a no-op.
			CLOGS(LIFECYCLE, "calling s2n_cleanup_final");
			if (-1 == s2n_cleanup_final())
				Tcl_Panic("s2n_cleanup_final failed: %s\n", s2n_strerror(s2n_errno, "EN"));
			g_init = 0;
		}
		Tcl_MutexUnlock(&g_init_mutex);
		Tcl_MutexFinalize(&g_init_mutex);
	}

	CLOGS(LIFECYCLE, "<-- unloading %s", clogs_name(interp));
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
