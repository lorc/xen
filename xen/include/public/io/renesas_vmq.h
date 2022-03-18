/******************************************************************************
 * renesas_vmq.h
 *
 * Unified display device I/O interface for Xen guest OSes.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (C) 2016-2017 EPAM Systems Inc.
 *
 * Authors: Oleksandr Andrushchenko <oleksandr_andrushchenko@epam.com>
 *          Oleksandr Grytsov <oleksandr_grytsov@epam.com>
 */

#ifndef __XEN_PUBLIC_IO_RENESAS_VMQ_H__
#define __XEN_PUBLIC_IO_RENESAS_VMQ_H__

#include "ring.h"
#include "../grant_table.h"

/*
 ******************************************************************************
 *                           Protocol version
 ******************************************************************************
 */
#define XENDISPL_PROTOCOL_VERSION     "1"
#define XENDISPL_PROTOCOL_VERSION_INT  1

/*
 ******************************************************************************
 *                  Main features provided by the protocol
 ******************************************************************************
 * This protocol aims to provide a vendor-specific protocol to share
 * Renesas R-Switch2 queues between domaincs
 *
 ******************************************************************************
 *                  Feature and Parameter Negotiation
 ******************************************************************************
 *
 * Front->back notifications: when enqueuing a new request, sending a
 * notification can be made conditional on xendispl_req (i.e., the generic
 * hold-off mechanism provided by the ring macros). Backends must set
 * xendispl_req appropriately (e.g., using RING_FINAL_CHECK_FOR_REQUESTS()).
 *
 * Back->front notifications: when enqueuing a new response, sending a
 * notification can be made conditional on xendispl_resp (i.e., the generic
 * hold-off mechanism provided by the ring macros). Frontends must set
 * xendispl_resp appropriately (e.g., using RING_FINAL_CHECK_FOR_RESPONSES()).
 *
 * The two halves of a para-virtual display driver utilize nodes within
 * XenStore to communicate capabilities and to negotiate operating parameters.
 * This section enumerates these nodes which reside in the respective front and
 * backend portions of XenStore, following the XenBus convention.
 *
 * All data in XenStore is stored as strings. Nodes specifying numeric
 * values are encoded in decimal. Integer value ranges listed below are
 * expressed as fixed sized integer types capable of storing the conversion
 * of a properly formated node string, without loss of information.
 *
 ******************************************************************************
 *                        Example configuration
 ******************************************************************************
 *
 * Note: depending on the use-case backend can expose more display connectors
 * than the underlying HW physically has by employing SW graphics compositors
 *
 * This is an example of backend and frontend configuration:
 *
 *--------------------------------- Backend -----------------------------------
 *
 * /local/domain/0/backend/renesas_vmq/1/0/frontend-id = "1"
 * /local/domain/0/backend/renesas_vmq/1/0/frontend = "/local/domain/1/device/renesas_vmq/0"
 * /local/domain/0/backend/renesas_vmq/1/0/state = "4"
 * /local/domain/0/backend/renesas_vmq/1/0/versions = "1"
 * /local/domain/0/backend/renesas_vmq/1/0/type = "tsn"
 * /local/domain/0/backend/renesas_vmq/1/0/if_num = "1"
 *
 *--------------------------------- Frontend ----------------------------------
 *
 * /local/domain/1/device/renesas_vmq/0/backend-id = "0"
 * /local/domain/1/device/renesas_vmq/0/backend = "/local/domain/0/backend/renesas_vmq/1/0"
 * /local/domain/1/device/renesas_vmq/0/state = "4"
 * /local/domain/1/device/renesas_vmq/0/version = "1"
 *
 ******************************************************************************
 *                            Backend XenBus Nodes
 ******************************************************************************
 *
 *----------------------------- Protocol version ------------------------------
 *
 * versions
 *      Values:         <string>
 *
 *      List of XENDISPL_LIST_SEPARATOR separated protocol versions supported
 *      by the backend. For example "1,2,3".
 *
 ******************************************************************************
 *                            Frontend XenBus Nodes
 ******************************************************************************
 *
 *-------------------------------- Addressing ---------------------------------
 *
 * dom-id
 *      Values:         <uint16_t>
 *
 *      Domain identifier.
 *
 * dev-id
 *      Values:         <uint16_t>
 *
 *      Device identifier.
 *
 *
 *----------------------------- Protocol version ------------------------------
 *
 * version
 *      Values:         <string>
 *
 *      Protocol version, chosen among the ones supported by the backend.
 *
 *------------------------- Interface type and id --- -------------------------
 *
 * type
 *      Values:         "tsn", "vmq"
 *
 *     "tsn" denotes direct access to ethernet IF
 *
 *     "vmq" makes backed to create virtual interface vmq%d
 *
 * "if_num"
 *      Values:         <uint16_t>
 *
 *      id (e.g. tsn1 or vmq4) of network interface
 */

/*
 ******************************************************************************
 *                               STATE DIAGRAMS
 ******************************************************************************
 *
 * Tool stack creates front and back state nodes with initial state
 * XenbusStateInitialising.
 * Tool stack creates and sets up frontend display configuration
 * nodes per domain.
 *
 *-------------------------------- Normal flow --------------------------------
 *
 * Front                                Back
 * =================================    =====================================
 * XenbusStateInitialising              XenbusStateInitialising
 *                                       o Query backend device identification
 *                                         data.
 *                                       o Open and validate backend device.
 *                                                |
 *                                                |
 *                                                V
 *                                      XenbusStateInitWait
 *
 * o Query frontend configuration
 * o Allocate and initialize
 *   event channels per configured
 *   connector.
 * o Publish transport parameters
 *   that will be in effect during
 *   this connection.
 *              |
 *              |
 *              V
 * XenbusStateInitialised
 *
 *                                       o Query frontend transport parameters.
 *                                       o Connect to the event channels.
 *                                                |
 *                                                |
 *                                                V
 *                                      XenbusStateConnected
 *
 *  o Create and initialize OS
 *    virtual display connectors
 *    as per configuration.
 *              |
 *              |
 *              V
 * XenbusStateConnected
 *
 *                                      XenbusStateUnknown
 *                                      XenbusStateClosed
 *                                      XenbusStateClosing
 * o Remove virtual display device
 * o Remove event channels
 *              |
 *              |
 *              V
 * XenbusStateClosed
 *
 *------------------------------- Recovery flow -------------------------------
 *
 * In case of frontend unrecoverable errors backend handles that as
 * if frontend goes into the XenbusStateClosed state.
 *
 * In case of backend unrecoverable errors frontend tries removing
 * the virtualized device. If this is possible at the moment of error,
 * then frontend goes into the XenbusStateInitialising state and is ready for
 * new connection with backend. If the virtualized device is still in use and
 * cannot be removed, then frontend goes into the XenbusStateReconfiguring state
 * until either the virtualized device is removed or backend initiates a new
 * connection. On the virtualized device removal frontend goes into the
 * XenbusStateInitialising state.
 *
 * Note on XenbusStateReconfiguring state of the frontend: if backend has
 * unrecoverable errors then frontend cannot send requests to the backend
 * and thus cannot provide functionality of the virtualized device anymore.
 * After backend is back to normal the virtualized device may still hold some
 * state: configuration in use, allocated buffers, client application state etc.
 * In most cases, this will require frontend to implement complex recovery
 * reconnect logic. Instead, by going into XenbusStateReconfiguring state,
 * frontend will make sure no new clients of the virtualized device are
 * accepted, allow existing client(s) to exit gracefully by signaling error
 * state etc.
 * Once all the clients are gone frontend can reinitialize the virtualized
 * device and get into XenbusStateInitialising state again signaling the
 * backend that a new connection can be made.
 *
 * There are multiple conditions possible under which frontend will go from
 * XenbusStateReconfiguring into XenbusStateInitialising, some of them are OS
 * specific. For example:
 * 1. The underlying OS framework may provide callbacks to signal that the last
 *    client of the virtualized device has gone and the device can be removed
 * 2. Frontend can schedule a deferred work (timer/tasklet/workqueue)
 *    to periodically check if this is the right time to re-try removal of
 *    the virtualized device.
 * 3. By any other means.
 *
 ******************************************************************************
 *                             REQUEST CODES
 ******************************************************************************
 * Request codes [0; 15] are reserved and must not be used
 */

#define XEN_RENESAS_VMQ_TX            0x10

/*
 ******************************************************************************
 *                                 EVENT CODES
 ******************************************************************************
 */
#define XEN_RENESAS_VMQ_RX          0x00

/*
 ******************************************************************************
 *               XENSTORE FIELD AND PATH NAME STRINGS, HELPERS
 ******************************************************************************
 */
#define XEN_RENESAS_VMQ_DRIVER_NAME          "renesas_vmq"

#define XEN_RENESAS_VMQ_FIELD_TX_RING_REF    "tx-ring-ref"
#define XEN_RENESAS_VMQ_FIELD_TX_CHANNEL     "tx-event-channel"
#define XEN_RENESAS_VMQ_FIELD_RX_RING_REF    "rx-ring-ref"
#define XEN_RENESAS_VMQ_FIELD_RX_CHANNEL     "rx-event-channel"
#define XEN_RENESAS_VMQ_FIELD_UNIQUE_ID      "unique-id"
#define XEN_RENESAS_VMQ_FIELD_TYPE           "type"
#define XEN_RENESAS_VMQ_FIELD_IF_NUM         "if-num"
#define XEN_RENESAS_VMQ_FIELD_OSID           "osid"

#endif /* __XEN_PUBLIC_IO_RENESAS_VMQ_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
