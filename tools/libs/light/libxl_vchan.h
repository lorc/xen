/*
    Common definitions for JSON messages processing by vchan
    Copyright (C) 2021 EPAM Systems Inc.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef LIBXL_VCHAN_H
#define LIBXL_VCHAN_H

#include <libxenvchan.h>

#define VCHAN_SERVER 1
#define VCHAN_CLIENT 0

#define END_OF_MESSAGE "\r\n"

#define VCHAN_MSG_EXECUTE         "execute"
#define VCHAN_MSG_RETURN          "return"
#define VCHAN_MSG_ERROR           "error"

struct vchan_state {
    /* Server domain ID. */
    libxl_domid domid;
    /* XenStore path of the server with the ring buffer and event channel. */
    char *xs_path;
    struct libxenvchan *ctrl;
    int select_fd;
    /* receive buffer */
    char *rx_buf;
    size_t rx_buf_size; /* current allocated size */
    size_t rx_buf_used; /* actual data in the buffer */
};

typedef int (*vchan_handle_t)(libxl__gc *gc, const libxl__json_object *request,
                              libxl__json_object **result);
typedef char* (*vchan_prepare_t)(libxl__gc *gc, const char *cmd,
                                 libxl__json_object *args, int id);
struct vchan_info {
    struct vchan_state *state;
    vchan_handle_t handle_msg;
    vchan_prepare_t prepare_cmd;
    /* buffer info */
    size_t receive_buf_size;
    size_t max_buf_size;
};

void libxl__vchan_param_add_string(libxl__gc *gc, libxl__json_object **param,
                                   const char *name, const char *s);
void libxl__vchan_param_add_integer(libxl__gc *gc, libxl__json_object **param,
                                    const char *name, const long long i);
int xs_path_exists(libxl__gc *gc, const char *xs_path);
libxl_domid vchan_find_server(libxl__gc *gc, char *xs_dir, char *xs_path);
struct vchan_state *vchan_get_instance(libxl__gc *gc, libxl_domid domid,
                                       char *vchan_xs_path, int is_server);
libxl__json_object *vchan_send_command(libxl__gc *gc, struct vchan_info *vchan,
                                       const char *cmd, libxl__json_object *args);
int vchan_process_command(libxl__gc *gc, struct vchan_info *vchan);

#endif /* LIBXL_VCHAN_H */

/*
 * Local variables:
 *  mode: C
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
