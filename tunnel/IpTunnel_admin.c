/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "admin/Admin.h"
#include "benc/String.h"
#include "benc/Int.h"
#include "benc/Dict.h"
#include "crypto/Key.h"
#include "memory/Allocator.h"
#include "memory/BufferAllocator.h"
#include "tunnel/IpTunnel.h"
#include "tunnel/IpTunnel_admin.h"

#include <stddef.h>

struct Context
{
    struct IpTunnel* ipTun;
    struct Admin* admin;
};

static void sendResponse(int conn, String* txid, struct Admin* admin)
{
    Dict resp = Dict_CONST(
        String_CONST("error"), String_OBJ(String_CONST("none")), Dict_CONST(
        String_CONST("connection"), Int_OBJ(conn), NULL
    ));
    Admin_sendMessage(&resp, txid, admin);
}

static void sendError(char* error, String* txid, struct Admin* admin)
{
    Dict resp = Dict_CONST(
        String_CONST("error"), String_OBJ(String_CONST(error)), NULL
    );
    Admin_sendMessage(&resp, txid, admin);
}

static void allowConnection(Dict* args, void* vcontext, String* txid)
{
    struct Context* context = (struct Context*) vcontext;
    String* publicKeyOfAuthorizedNode =
        Dict_getString(args, String_CONST("publicKeyOfAuthorizedNode"));
    String* ip6Address = Dict_getString(args, String_CONST("ip6Address"));
    String* ip4Address = Dict_getString(args, String_CONST("ip4Address"));
    uint8_t pubKey[32];
    uint8_t ip6Addr[16];

    uint8_t ip6ToGive[16];
    uint8_t ip4ToGive[4];

    char* error;
    int ret;
    if (!ip6Address && !ip4Address) {
        error = "Must specify ip6Address or ip4Address";
    } else if ((ret = Key_parse(publicKeyOfAuthorizedNode, pubKey, ip6Addr)) != 0) {
        error = Key_parse_strerror(ret);
    } else if (ip6Address && evutil_inet_pton(AF_INET6, ip6Address->bytes, ip6ToGive) < 1) {
        error = "malformed ip6Address";
    } else if (ip4Address && evutil_inet_pton(AF_INET, ip4Address->bytes, ip4ToGive) < 1) {
        error = "malformed ip4Address";
    } else {
        int conn = IpTunnel_allowConnection(pubKey,
                                            (ip6Address) ? ip6ToGive : NULL,
                                            (ip4Address) ? ip4ToGive : NULL,
                                            context->ipTun);
        sendResponse(conn, txid, context->admin);
        return;
    }

    sendError(error, txid, context->admin);
}


static void connectTo(Dict* args, void* vcontext, String* txid)
{
    struct Context* context = vcontext;
    String* publicKeyOfNodeToConnectTo =
        Dict_getString(args, String_CONST("publicKeyOfNodeToConnectTo"));

    uint8_t pubKey[32];
    uint8_t ip6[16];
    int ret;
    if ((ret = Key_parse(publicKeyOfNodeToConnectTo, pubKey, ip6)) != 0) {
        sendError(Key_parse_strerror(ret), txid, context->admin);
        return;
    }
    int conn = IpTunnel_connectTo(pubKey, context->ipTun);
    sendResponse(conn, txid, context->admin);
}

static void removeConnection(Dict* args, void* vcontext, String* txid)
{
    struct Context* context = vcontext;

    int conn = (int) *(Dict_getInt(args, String_CONST("connection")));
    char* error = "none";
    if (IpTunnel_removeConnection_NOT_FOUND == IpTunnel_removeConnection(conn, context->ipTun)) {
        error = "not found";
    }
    sendError(error, txid, context->admin);
}

static void listConnections(Dict* args, void* vcontext, String* txid)
{
    struct Context* context = vcontext;
    struct Allocator* alloc;
    BufferAllocator_STACK(alloc, 1024);
    List* l = NULL;
    for (int i = 0; i < (int)context->ipTun->connectionList.count; i++) {
        l = List_addInt(l, context->ipTun->connectionList.connections[i].number, alloc);
    }
    Dict resp = Dict_CONST(
        String_CONST("connections"), List_OBJ(l), Dict_CONST(
        String_CONST("error"), String_OBJ(String_CONST("none")), NULL
    ));
    Admin_sendMessage(&resp, txid, context->admin);
}

static void showConn(struct IpTunnel_Connection* conn, String* txid, struct Admin* admin)
{
    struct Allocator* alloc;
    BufferAllocator_STACK(alloc, 1024);
    Dict* d = Dict_new(alloc);

    char ip6[40];
    if (!Bits_isZero(conn->connectionIp6, 16)) {
        Assert_always(evutil_inet_ntop(AF_INET6, conn->connectionIp6, ip6, 40));
        Dict_putString(d, String_CONST("ip6Address"), String_CONST(ip6), alloc);
    }

    char ip4[16];
    if (!Bits_isZero(conn->connectionIp4, 4)) {
        Assert_always(evutil_inet_ntop(AF_INET, conn->connectionIp4, ip4, 16));
        Dict_putString(d, String_CONST("ip4Address"), String_CONST(ip4), alloc);
    }

    Dict_putString(d, String_CONST("key"), Key_stringify(conn->header.nodeKey, alloc), alloc);
    Dict_putInt(d, String_CONST("outgoing"), conn->isOutgoing, alloc);

    Admin_sendMessage(d, txid, admin);
}

static void showConnection(Dict* args, void* vcontext, String* txid)
{
    struct Context* context = vcontext;
    int connNum = (int) *(Dict_getInt(args, String_CONST("connection")));

    for (int i = 0; i < (int)context->ipTun->connectionList.count; i++) {
        if (connNum == context->ipTun->connectionList.connections[i].number) {
            showConn(&context->ipTun->connectionList.connections[i], txid, context->admin);
            return;
        }
    }
    sendError("connection not found", txid, context->admin);
}

void IpTunnel_admin_register(struct IpTunnel* ipTun, struct Admin* admin, struct Allocator* alloc)
{
    struct Context* context = alloc->clone(sizeof(struct Context), alloc, &(struct Context) {
        .admin = admin,
        .ipTun = ipTun
    });

    Admin_registerFunction("IpTunnel_allowConnection", allowConnection, context, true,
        ((struct Admin_FunctionArg[]) {
            { .name = "publicKeyOfAuthorizedNode", .required = 1, .type = "String" },
            { .name = "ip6Address", .required = 0, .type = "String" },
            { .name = "ip4Address", .required = 0, .type = "String" },
        }), admin);

    Admin_registerFunction("IpTunnel_connectTo", connectTo, context, true,
        ((struct Admin_FunctionArg[]) {
            { .name = "publicKeyOfNodeToConnectTo", .required = 1, .type = "String" }
        }), admin);

    Admin_registerFunction("IpTunnel_removeConnection", removeConnection, context, true,
        ((struct Admin_FunctionArg[]) {
            { .name = "connection", .required = 1, .type = "Int" }
        }), admin);

    Admin_registerFunction("IpTunnel_showConnection", showConnection, context, true,
        ((struct Admin_FunctionArg[]) {
            { .name = "connection", .required = 1, .type = "Int" }
        }), admin);

    Admin_registerFunction("IpTunnel_listConnections", listConnections, context, true, NULL, admin);
}