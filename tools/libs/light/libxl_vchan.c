/*
 * Vchan support for JSON messages processing
 *
 * Copyright (C) 2021 EPAM Systems Inc.
 * Author: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 * Author: Anastasiia Lukianenko <anastasiia_lukianenko@epam.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"
#include "libxl_vchan.h"

/* Based on QMP Parameters Helpers */
static void vchan_parameters_common_add(libxl__gc *gc, libxl__json_object **param,
                                        const char *name, libxl__json_object *obj)
{
    libxl__json_map_node *arg = NULL;

    if (!*param) {
        *param = libxl__json_object_alloc(gc, JSON_MAP);
    }

    GCNEW(arg);

    arg->map_key = libxl__strdup(gc, name);
    arg->obj = obj;

    flexarray_append((*param)->u.map, arg);
}


void libxl__vchan_param_add_string(libxl__gc *gc, libxl__json_object **param,
                                   const char *name, const char *s)
{
    libxl__json_object *obj;

    obj = libxl__json_object_alloc(gc, JSON_STRING);
    obj->u.string = libxl__strdup(gc, s);

    vchan_parameters_common_add(gc, param, name, obj);
}

void libxl__vchan_param_add_integer(libxl__gc *gc, libxl__json_object **param,
                                    const char *name, const long long i)
{
    libxl__json_object *obj;

    obj = libxl__json_object_alloc(gc, JSON_INTEGER);
    obj->u.i = i;

    vchan_parameters_common_add(gc, param, name, obj);
}

/* Returns 1 if path exists, 0 if not, ERROR_* (<0) on error. */
int xs_path_exists(libxl__gc *gc, const char *xs_path)
{
    int rc;
    const char *dir;

    rc = libxl__xs_read_checked(gc, XBT_NULL, xs_path, &dir);
    if (rc)
        return rc;
    if (dir)
        return 1;
    return 0;
}

libxl_domid vchan_find_server(libxl__gc *gc, char *xs_dir, char *xs_path)
{
    char **domains;
    unsigned int i, n;
    libxl_domid domid = DOMID_INVALID;

    domains = libxl__xs_directory(gc, XBT_NULL, "/local/domain", &n);
    if (!n)
        goto out;

    for (i = 0; i < n; i++) {
        int d;

        if (sscanf(domains[i], "%d", &d) != 1)
            continue;
        if (xs_path_exists(gc, GCSPRINTF("%s%d%s", xs_dir, d, xs_path)) > 0) {
            /* Found the domain where the server lives. */
            domid = d;
            break;
        }
    }

out:
    return domid;
}

static int vchan_init_client(libxl__gc *gc, struct vchan_state *state, int is_server)
{
	if (is_server) {
		state->ctrl = libxenvchan_server_init(NULL, state->domid, state->xs_path, 0, 0);
	    if (!state->ctrl) {
	        perror("Libxenvchan server init failed\n");
	        exit(1);
	    }
	} else {
		state->ctrl = libxenvchan_client_init(CTX->lg, state->domid,
                                          state->xs_path);
	    if (!state->ctrl) {
	        LOGE(ERROR, "Couldn't intialize vchan client");
	        return ERROR_FAIL;
	    }
	}

    state->select_fd = libxenvchan_fd_for_select(state->ctrl);
    if (state->select_fd < 0) {
        LOGE(ERROR, "Couldn't read file descriptor for vchan client");
        return ERROR_FAIL;
    }

    LOG(DEBUG, "Intialized vchan client, server at %s", state->xs_path);

    return 0;
}

/*
 * TODO: Running this code in multi-threaded environment
 * The code now assumes that there is only one client invocation process
 * in one domain. In the future, it is necessary to take into account cases
 * when within one domain there will be several requests from a client at the
 * same time. Therefore, it will be necessary to regulate the multithreading
 * of processes.
 */
struct vchan_state *vchan_get_instance(libxl__gc *gc, libxl_domid domid,
                                       char *vchan_xs_path, int is_server)
{
    static struct vchan_state *state = NULL;
    int ret;

    if (state)
        return state;

    state = libxl__zalloc(gc, sizeof(*state));
    state->domid = domid;
    state->xs_path = vchan_xs_path;
    ret = vchan_init_client(gc, state, is_server);
    if (ret)
        state = NULL;

    return state;
}

/*
 * Find a JSON object and store it in o_r.
 * return ERROR_NOTFOUND if no object is found.
 */
static int vchan_get_next_msg(libxl__gc *gc, struct vchan_state *state,
                              libxl__json_object **o_r)
{
    size_t len;
    char *end = NULL;
    const size_t eoml = sizeof(END_OF_MESSAGE) - 1;
    libxl__json_object *o = NULL;

    if (!state->rx_buf_used)
        return ERROR_NOTFOUND;

    /* Search for the end of a message: "\r\n" */
    end = memmem(state->rx_buf, state->rx_buf_used, END_OF_MESSAGE, eoml);
    if (!end)
        return ERROR_NOTFOUND;
    len = (end - state->rx_buf) + eoml;

    LOGD(DEBUG, state->domid, "parsing %zuB: '%.*s'", len, (int)len,
         state->rx_buf);

    /* Replace \r by \0 so that libxl__json_parse can use strlen */
    state->rx_buf[len - eoml] = '\0';
    o = libxl__json_parse(gc, state->rx_buf);

    if (!o) {
        LOGD(ERROR, state->domid, "Parse error");
        return ERROR_FAIL;
    }

    state->rx_buf_used -= len;
    memmove(state->rx_buf, state->rx_buf + len, state->rx_buf_used);

    LOGD(DEBUG, state->domid, "JSON object received: %s", JSON(o));

    *o_r = o;

    return 0;
}

static libxl__json_object *vchan_handle_message(libxl__gc *gc,
                                                struct vchan_info *vchan,
                                                const libxl__json_object *request)
{
	libxl__json_object *result = NULL;
	const libxl__json_object *command_obj;
	int ret;

	ret = vchan->handle_msg(gc, request, &result);
	if (ret == ERROR_FAIL) {
		LOGE(ERROR, "Message handling failed\n");
	} else if (ret == ERROR_NOTFOUND) {
		command_obj = libxl__json_map_get(VCHAN_MSG_EXECUTE, request, JSON_ANY);
		LOGE(ERROR, "Unknown command: %s\n", command_obj->u.string);
	}
    return result;
}

static int set_nonblocking(int fd, int nonblocking)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1)
        return -1;

    if (nonblocking)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1)
        return -1;

    return 0;
}

static libxl__json_object *vchan_process_request(libxl__gc *gc,
                                                 struct vchan_info *vchan)
{
    int rc, ret;
    ssize_t r;
    fd_set rfds;
    fd_set wfds;

    while (true) {
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(vchan->state->select_fd, &rfds);
        ret = select(vchan->state->select_fd + 1, &rfds, &wfds, NULL, NULL);
        if (ret < 0) {
            LOGE(ERROR, "Error occured during the libxenvchan fd monitoring\n");
            return NULL;
        }
        if (FD_ISSET(vchan->state->select_fd, &rfds))
            libxenvchan_wait(vchan->state->ctrl);
        /* Check if the buffer still have space, or increase size */
        if (vchan->state->rx_buf_size - vchan->state->rx_buf_used < vchan->receive_buf_size) {
            size_t newsize = vchan->state->rx_buf_size * 2 + vchan->receive_buf_size;

            if (newsize > vchan->max_buf_size) {
                LOGD(ERROR, vchan->state->domid,
                     "receive buffer is too big (%zu > %zu)",
                     newsize, vchan->max_buf_size);
                return NULL;
            }
            vchan->state->rx_buf_size = newsize;
            vchan->state->rx_buf = libxl__realloc(gc, vchan->state->rx_buf,
                                                  vchan->state->rx_buf_size);
        }

        while (libxenvchan_data_ready(vchan->state->ctrl)) {
            r = libxenvchan_read(vchan->state->ctrl,
                                 vchan->state->rx_buf + vchan->state->rx_buf_used,
                                 vchan->state->rx_buf_size - vchan->state->rx_buf_used);
            if (r < 0) {
                LOGED(ERROR, vchan->state->domid, "error reading");
                return NULL;
            }

            LOG(DEBUG, "received %zdB: '%.*s'", r,
                (int)r, vchan->state->rx_buf + vchan->state->rx_buf_used);

            vchan->state->rx_buf_used += r;
            assert(vchan->state->rx_buf_used <= vchan->state->rx_buf_size);

            libxl__json_object *o = NULL;
            /* parse rx buffer to find one json object */
            rc = vchan_get_next_msg(gc, vchan->state, &o);
            if (rc == ERROR_NOTFOUND)
                break;
            else if (rc)
                return NULL;

            return vchan_handle_message(gc, vchan, o);
        }
        if ( !libxenvchan_is_open(vchan->state->ctrl)) {
            if (set_nonblocking(1, 0))
                return NULL;
        }
    }
    return NULL;
}

static int vchan_write(libxl__gc *gc, struct vchan_state *state, char *cmd)
{
    size_t len;
    int ret;

    len = strlen(cmd);
    while (len) {
        ret = libxenvchan_write(state->ctrl, cmd, len);
        if (ret < 0) {
            LOGE(ERROR, "vchan write failed");
            return ERROR_FAIL;
        }
        cmd += ret;
        len -= ret;
    }
    return 0;
}

libxl__json_object *vchan_send_command(libxl__gc *gc, struct vchan_info *vchan,
                                       const char *cmd, libxl__json_object *args)
{
    libxl__json_object *result;
    char *json;
    int ret;

    json = vchan->prepare_cmd(gc, cmd, args, 0);
    if (!json)
        return NULL;

    ret = vchan_write(gc, vchan->state, json);
    if (ret < 0)
        return NULL;

    result = vchan_process_request(gc, vchan);
    return result;
}

int vchan_process_command(libxl__gc *gc, struct vchan_info *vchan)
{
    libxl__json_object *result;
    char *json;
    int ret;

    result = vchan_process_request(gc, vchan);

    json = vchan->prepare_cmd(gc, NULL, result, 0);
    if (!json)
        return -1;

    ret = vchan_write(gc, vchan->state, json);
    if (ret < 0)
        return -1;

    return 0;
}
