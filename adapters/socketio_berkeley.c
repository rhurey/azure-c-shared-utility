// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#define SOCKETIO_BERKELEY_UNDEF_BSD_SOURCE
#endif

#define _DEFAULT_SOURCE
#include <net/if.h>
#undef _DEFAULT_SOURCE

#ifdef SOCKETIO_BERKELEY_UNDEF_BSD_SOURCE
#undef _BSD_SOURCE
#undef SOCKETIO_BERKELEY_UNDEF_BSD_SOURCE
#endif

#include <signal.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "azure_c_shared_utility/socketio.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#ifdef TIZENRT
#include <net/lwip/tcp.h>
#else
#include <netinet/tcp.h>
#endif
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/gbnetwork.h"
#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/optionhandler.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/const_defines.h"
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SOCKET_SUCCESS                 0
#define INVALID_SOCKET                 -1
#define MAC_ADDRESS_STRING_LENGTH      18

#ifndef IFREQ_BUFFER_SIZE
#define IFREQ_BUFFER_SIZE              1024
#endif

#define CONNECT_TIMEOUT_SECONDS 10

typedef enum IO_STATE_TAG
{
    IO_STATE_CLOSED,
    IO_STATE_OPENING,
    IO_STATE_OPEN,
    IO_STATE_CLOSING,
    IO_STATE_ERROR
} IO_STATE;

typedef struct PENDING_SOCKET_IO_TAG
{
    unsigned char* bytes;
    size_t size;
    ON_SEND_COMPLETE on_send_complete;
    void* callback_context;
    SINGLYLINKEDLIST_HANDLE pending_io_list;
} PENDING_SOCKET_IO;

typedef struct SOCKET_IO_INSTANCE_TAG
{
    int socket;
    ON_BYTES_RECEIVED on_bytes_received;
    ON_IO_ERROR on_io_error;
    void* on_bytes_received_context;
    void* on_io_error_context;
    char* hostname;
    int port;
    char* target_mac_address;
    IO_STATE io_state;
    SINGLYLINKEDLIST_HANDLE pending_io_list;
    unsigned char recv_bytes[RECEIVE_BYTES_VALUE];
} SOCKET_IO_INSTANCE;

typedef struct NETWORK_INTERFACE_DESCRIPTION_TAG
{
    char* name;
    char* mac_address;
    char* ip_address;
    struct NETWORK_INTERFACE_DESCRIPTION_TAG* next;
} NETWORK_INTERFACE_DESCRIPTION;

/*this function will clone an option given by name and value*/
static void* socketio_CloneOption(const char* name, const void* value)
{
    void* result;

    if (name != NULL)
    {
        result = NULL;

        if (strcmp(name, OPTION_NET_INT_MAC_ADDRESS) == 0)
        {
            if (value == NULL)
            {
                LogError("Failed cloning option %s (value is NULL)", name);
            }
            else
            {
                if ((result = malloc(sizeof(char) * (strlen((char*)value) + 1))) == NULL)
                {
                    LogError("Failed cloning option %s (malloc failed)", name);
                }
                else if (strcpy((char*)result, (char*)value) == NULL)
                {
                    LogError("Failed cloning option %s (strcpy failed)", name);
                    free(result);
                    result = NULL;
                }
            }
        }
        else
        {
            LogError("Cannot clone option %s (not suppported)", name);
        }
    }
    else
    {
        result = NULL;
    }
    return result;
}

/*this function destroys an option previously created*/
static void socketio_DestroyOption(const char* name, const void* value)
{
    if (name != NULL)
    {
        if (strcmp(name, OPTION_NET_INT_MAC_ADDRESS) == 0 && value != NULL)
        {
            free((void*)value);
        }
    }
}

static OPTIONHANDLER_HANDLE socketio_retrieveoptions(CONCRETE_IO_HANDLE handle)
{
    OPTIONHANDLER_HANDLE result;

    if (handle == NULL)
    {
        LogError("failed retrieving options (handle is NULL)");
        result = NULL;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)handle;

        result = OptionHandler_Create(socketio_CloneOption, socketio_DestroyOption, socketio_setoption);
        if (result == NULL)
        {
            LogError("unable to OptionHandler_Create");
        }
        else if (socket_io_instance->target_mac_address != NULL &&
            OptionHandler_AddOption(result, OPTION_NET_INT_MAC_ADDRESS, socket_io_instance->target_mac_address) != OPTIONHANDLER_OK)
        {
            LogError("failed retrieving options (failed adding net_interface_mac_address)");
            OptionHandler_Destroy(result);
            result = NULL;
        }
    }

    return result;
}

static const IO_INTERFACE_DESCRIPTION socket_io_interface_description = 
{
    socketio_retrieveoptions,
    socketio_create,
    socketio_destroy,
    socketio_open,
    socketio_close,
    socketio_send,
    socketio_dowork,
    socketio_setoption
};

static void indicate_error(SOCKET_IO_INSTANCE* socket_io_instance)
{
    if (socket_io_instance->on_io_error != NULL)
    {
        socket_io_instance->on_io_error(socket_io_instance->on_io_error_context);
    }
}

static int add_pending_io(SOCKET_IO_INSTANCE* socket_io_instance, const unsigned char* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result;
    PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)malloc(sizeof(PENDING_SOCKET_IO));
    if (pending_socket_io == NULL)
    {
        result = __FAILURE__;
    }
    else
    {
        pending_socket_io->bytes = (unsigned char*)malloc(size);
        if (pending_socket_io->bytes == NULL)
        {
            LogError("Allocation Failure: Unable to allocate pending list.");
            free(pending_socket_io);
            result = __FAILURE__;
        }
        else
        {
            pending_socket_io->size = size;
            pending_socket_io->on_send_complete = on_send_complete;
            pending_socket_io->callback_context = callback_context;
            pending_socket_io->pending_io_list = socket_io_instance->pending_io_list;
            (void)memcpy(pending_socket_io->bytes, buffer, size);

            if (singlylinkedlist_add(socket_io_instance->pending_io_list, pending_socket_io) == NULL)
            {
                LogError("Failure: Unable to add socket to pending list.");
                free(pending_socket_io->bytes);
                free(pending_socket_io);
                result = __FAILURE__;
            }
            else
            {
                result = 0;
            }
        }
    }
    return result;
}

static STATIC_VAR_UNUSED void signal_callback(int signum)
{
    AZURE_UNREFERENCED_PARAMETER(signum);
    LogError("Socket received signal %d.", signum);
}

#ifndef __APPLE__
static void destroy_network_interface_descriptions(NETWORK_INTERFACE_DESCRIPTION* nid)
{
    if (nid != NULL)
    {
        if (nid->next != NULL)
        {
            destroy_network_interface_descriptions(nid->next);
        }
    
        if (nid->name != NULL)
        {
            free(nid->name);
        }

        if (nid->mac_address != NULL)
        {
            free(nid->mac_address);
        }
        
        if (nid->ip_address != NULL)
        {
            free(nid->ip_address);
        }

        free(nid);
    }
}

static NETWORK_INTERFACE_DESCRIPTION* create_network_interface_description(struct ifreq *ifr, NETWORK_INTERFACE_DESCRIPTION* previous_nid)
{
    NETWORK_INTERFACE_DESCRIPTION* result;
    
    if ((result = (NETWORK_INTERFACE_DESCRIPTION*)malloc(sizeof(NETWORK_INTERFACE_DESCRIPTION))) == NULL)
    {
        LogError("Failed allocating NETWORK_INTERFACE_DESCRIPTION");
    }
    else if ((result->name = (char*)malloc(sizeof(char) * (strlen(ifr->ifr_name) + 1))) == NULL)
    {
        LogError("failed setting interface description name (malloc failed)");
        destroy_network_interface_descriptions(result);
        result = NULL;
    }
    else if (strcpy(result->name, ifr->ifr_name) == NULL)
    {
        LogError("failed setting interface description name (strcpy failed)");
        destroy_network_interface_descriptions(result);
        result = NULL;
    }
    else
    {
        char* ip_address;
        unsigned char* mac = (unsigned char*)ifr->ifr_hwaddr.sa_data;

        if ((result->mac_address = (char*)malloc(sizeof(char) * MAC_ADDRESS_STRING_LENGTH)) == NULL)
        {
            LogError("failed formatting mac address (malloc failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if (sprintf(result->mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]) <= 0)
        {
            LogError("failed formatting mac address (sprintf failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if ((ip_address = inet_ntoa(((struct sockaddr_in*)&ifr->ifr_addr)->sin_addr)) == NULL)
        {
            LogError("failed setting the ip address (inet_ntoa failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if ((result->ip_address = (char*)malloc(sizeof(char) * (strlen(ip_address) + 1))) == NULL)
        {
            LogError("failed setting the ip address (malloc failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else if (strcpy(result->ip_address, ip_address) == NULL)
        {
            LogError("failed setting the ip address (strcpy failed)");
            destroy_network_interface_descriptions(result);
            result = NULL;
        }
        else
        {
            result->next = NULL;

            if (previous_nid != NULL)
            {
                previous_nid->next = result;
            }
        }
    }
    
    return result;
}

static int get_network_interface_descriptions(int socket, NETWORK_INTERFACE_DESCRIPTION** nid)
{
    int result;

    struct ifreq ifr;
    struct ifconf ifc;
    char buf[IFREQ_BUFFER_SIZE];

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;

    if (ioctl(socket, SIOCGIFCONF, &ifc) == -1)
    {
        LogError("ioctl failed querying socket (SIOCGIFCONF, errno=%s)", errno);
        result = __FAILURE__;
    }
    else 
    {
        NETWORK_INTERFACE_DESCRIPTION* root_nid = NULL;
        NETWORK_INTERFACE_DESCRIPTION* new_nid = NULL;

        struct ifreq* it = ifc.ifc_req;
        const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));
        
        result = 0;

        for (; it != end; ++it)
        {
            strcpy(ifr.ifr_name, it->ifr_name);

            if (ioctl(socket, SIOCGIFFLAGS, &ifr) != 0)
            {
                LogError("ioctl failed querying socket (SIOCGIFFLAGS, errno=%d)", errno);
                result = __FAILURE__;
                break;
            }
            else if (ioctl(socket, SIOCGIFHWADDR, &ifr) != 0)
            {
                LogError("ioctl failed querying socket (SIOCGIFHWADDR, errno=%d)", errno);
                result = __FAILURE__;
                break;
            }
            else if (ioctl(socket, SIOCGIFADDR, &ifr) != 0)
            {
                LogError("ioctl failed querying socket (SIOCGIFADDR, errno=%d)", errno);
                result = __FAILURE__;
                break;
            }
            else if ((new_nid = create_network_interface_description(&ifr, new_nid)) == NULL)
            {
                LogError("Failed creating network interface description");
                result = __FAILURE__;
                break;    
            }
            else if (root_nid == NULL)
            {
                root_nid = new_nid;
            }
        }
        
        if (result == 0)
        {
            *nid = root_nid;
        }
        else
        {
            destroy_network_interface_descriptions(root_nid);
        }
    }

    return result;
}

static int set_target_network_interface(int socket, char* mac_address)
{
    int result;
    NETWORK_INTERFACE_DESCRIPTION* nid;

    if (get_network_interface_descriptions(socket, &nid) != 0)
    {
        LogError("Failed getting network interface descriptions");
        result = __FAILURE__;
    }
    else
    {
        NETWORK_INTERFACE_DESCRIPTION* current_nid = nid;
    
        while(current_nid != NULL)
        {
            if (strcmp(mac_address, current_nid->mac_address) == 0)
            {
                break;
            }

            current_nid = current_nid->next;
        }

        if (current_nid == NULL)
        {
            LogError("Did not find a network interface matching MAC ADDRESS");
            result = __FAILURE__;
        }
        else if (setsockopt(socket, SOL_SOCKET, SO_BINDTODEVICE, current_nid->name, strlen(current_nid->name)) != 0)
        {
            LogError("setsockopt failed (%d)", errno);
            result = __FAILURE__;
        }
        else
        {
            result = 0;
        }
        
        destroy_network_interface_descriptions(nid);
    }

    return result;
}
#endif //__APPLE__

CONCRETE_IO_HANDLE socketio_create(void* io_create_parameters)
{
    SOCKETIO_CONFIG* socket_io_config = io_create_parameters;
    SOCKET_IO_INSTANCE* result;

    if (socket_io_config == NULL)
    {
        LogError("Invalid argument: socket_io_config is NULL");
        result = NULL;
    }
    else
    {
        result = malloc(sizeof(SOCKET_IO_INSTANCE));
        if (result != NULL)
        {
            result->pending_io_list = singlylinkedlist_create();
            if (result->pending_io_list == NULL)
            {
                LogError("Failure: singlylinkedlist_create unable to create pending list.");
                free(result);
                result = NULL;
            }
            else
            {
                if (socket_io_config->hostname != NULL)
                {
                    result->hostname = (char*)malloc(strlen(socket_io_config->hostname) + 1);
                    if (result->hostname != NULL)
                    {
                        (void)strcpy(result->hostname, socket_io_config->hostname);
                    }

                    result->socket = INVALID_SOCKET;
                }
                else
                {
                    result->hostname = NULL;
                    result->socket = *((int*)socket_io_config->accepted_socket);
                }

                if ((result->hostname == NULL) && (result->socket == INVALID_SOCKET))
                {
                    LogError("Failure: hostname == NULL and socket is invalid.");
                    singlylinkedlist_destroy(result->pending_io_list);
                    free(result);
                    result = NULL;
                }
                else
                {
                    result->port = socket_io_config->port;
                    result->target_mac_address = NULL;
                    result->on_bytes_received = NULL;
                    result->on_io_error = NULL;
                    result->on_bytes_received_context = NULL;
                    result->on_io_error_context = NULL;
                    result->io_state = IO_STATE_CLOSED;
                }
            }
        }
        else
        {
            LogError("Allocation Failure: SOCKET_IO_INSTANCE");
        }
    }

    return result;
}

void socketio_destroy(CONCRETE_IO_HANDLE socket_io)
{
    if (socket_io != NULL)
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        /* we cannot do much if the close fails, so just ignore the result */
        if (socket_io_instance->socket != INVALID_SOCKET)
        {
            close(socket_io_instance->socket);
        }

        /* clear allpending IOs */
        LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        while (first_pending_io != NULL)
        {
            PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)singlylinkedlist_item_get_value(first_pending_io);
            if (pending_socket_io != NULL)
            {
                free(pending_socket_io->bytes);
                free(pending_socket_io);
            }

            (void)singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io);
            first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        }

        singlylinkedlist_destroy(socket_io_instance->pending_io_list);
        free(socket_io_instance->hostname);
        free(socket_io_instance->target_mac_address);
        free(socket_io);
    }
}

int socketio_open(CONCRETE_IO_HANDLE socket_io, ON_IO_OPEN_COMPLETE on_io_open_complete, void* on_io_open_complete_context, ON_BYTES_RECEIVED on_bytes_received, void* on_bytes_received_context, ON_IO_ERROR on_io_error, void* on_io_error_context)
{
    int result;
    int retval = -1;
    int poll_errno = 0;

    IO_OPEN_RESULT_DETAILED open_result_detailed = { IO_OPEN_OK, 0 };

    SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
    if (socket_io == NULL)
    {
        LogError("Invalid argument: SOCKET_IO_INSTANCE is NULL");
        result = open_result_detailed.code = __FAILURE__;
    }
    else
    {
        if (socket_io_instance->io_state != IO_STATE_CLOSED)
        {
            LogError("Failure: socket state is not closed.");
            result = open_result_detailed.code = __FAILURE__;
        }
        else if (socket_io_instance->socket != INVALID_SOCKET)
        {
            // Opening an accepted socket
            socket_io_instance->on_bytes_received_context = on_bytes_received_context;
            socket_io_instance->on_bytes_received = on_bytes_received;
            socket_io_instance->on_io_error = on_io_error;
            socket_io_instance->on_io_error_context = on_io_error_context;

            socket_io_instance->io_state = IO_STATE_OPEN;

            result = 0;
        }
        else
        {
            struct addrinfo* addrInfo;
            char portString[16];

            socket_io_instance->socket = socket(AF_INET, SOCK_STREAM, 0);
            if (socket_io_instance->socket < SOCKET_SUCCESS)
            {
                LogError("Failure: socket create failure %d.", socket_io_instance->socket);
                open_result_detailed.code = socket_io_instance->socket;
                result = __FAILURE__;
            }
#ifndef __APPLE__
            else if (socket_io_instance->target_mac_address != NULL &&
                     set_target_network_interface(socket_io_instance->socket, socket_io_instance->target_mac_address) != 0)
            {
                LogError("Failure: failed selecting target network interface (MACADDR=%s).", socket_io_instance->target_mac_address);
                close(socket_io_instance->socket);
                socket_io_instance->socket = INVALID_SOCKET;
                result = open_result_detailed.code = __FAILURE__;
            }
#endif //__APPLE__
            else
            {
                struct addrinfo addrHint = { 0 };
                addrHint.ai_family = AF_INET;
                addrHint.ai_socktype = SOCK_STREAM;
                addrHint.ai_protocol = 0;

                sprintf(portString, "%u", socket_io_instance->port);
                int err = getaddrinfo(socket_io_instance->hostname, portString, &addrHint, &addrInfo);
                if (err != 0)
                {
                    LogError("Failure: getaddrinfo failure %d.", err);
                    open_result_detailed.code = err;
                    close(socket_io_instance->socket);
                    socket_io_instance->socket = INVALID_SOCKET;
                    result = __FAILURE__;
                }
                else
                {
                    int flags;
                    if ((-1 == (flags = fcntl(socket_io_instance->socket, F_GETFL, 0))) ||
                        (fcntl(socket_io_instance->socket, F_SETFL, flags | O_NONBLOCK) == -1))
                    {
                        LogError("Failure: fcntl failure %d.", errno);
                        open_result_detailed.code = errno;
                        close(socket_io_instance->socket);
                        socket_io_instance->socket = INVALID_SOCKET;
                        result = __FAILURE__;
                    }
                    else
                    {
                        err = connect(socket_io_instance->socket, addrInfo->ai_addr, sizeof(*addrInfo->ai_addr));
                        if ((err != 0) && (errno != EINPROGRESS))
                        {
                            LogError("Failure: connect failure %d.", errno);
                            open_result_detailed.code = errno;
                            close(socket_io_instance->socket);
                            socket_io_instance->socket = INVALID_SOCKET;
                            result = __FAILURE__;
                        }
                        else
                        {
                            if (err != 0)
                            {
                                struct pollfd fd = { 0 };
                                fd.fd = socket_io_instance->socket;
                                // Wait until writing is possible.
                                fd.events = POLLOUT;

                                do
                                {
                                    retval = poll(&fd, 1, CONNECT_TIMEOUT_SECONDS * 1000);
                                    if (retval < 0)
                                    {
                                        poll_errno = errno;
                                    }
                                } while (retval < 0 && poll_errno == EINTR);

                                if (retval != 1)
                                {
                                    LogError("Failure: poll failure, retval %d, errno %d.", retval, poll_errno);
                                    if (retval == 0) {
                                        // timeout
                                        open_result_detailed.code = 9999;
                                    } else {
                                        open_result_detailed.code = poll_errno;
                                    }
                                    close(socket_io_instance->socket);
                                    socket_io_instance->socket = INVALID_SOCKET;
                                    result = __FAILURE__;
                                }
                                else
                                {
                                    int so_error = 0;
                                    socklen_t len = sizeof(so_error);
                                    err = getsockopt(socket_io_instance->socket, SOL_SOCKET, SO_ERROR, &so_error, &len);
                                    if (err != 0)
                                    {
                                        LogError("Failure: getsockopt failure %d.", errno);
                                        open_result_detailed.code = errno;
                                        close(socket_io_instance->socket);
                                        socket_io_instance->socket = INVALID_SOCKET;
                                        result = __FAILURE__;
                                    }
                                    else if (so_error != 0)
                                    {
                                        err = so_error;
                                        LogError("Failure: connect failure %d.", so_error);
                                        open_result_detailed.code = so_error;
                                        close(socket_io_instance->socket);
                                        socket_io_instance->socket = INVALID_SOCKET;
                                        result = __FAILURE__;
                                    }
                                    else
                                    {
                                        result = 0;
                                    }
                                }
                            }
                            else
                            {
                                result = 0;
                            }

                            if (err == 0)
                            {
                                socket_io_instance->on_bytes_received = on_bytes_received;
                                socket_io_instance->on_bytes_received_context = on_bytes_received_context;

                                socket_io_instance->on_io_error = on_io_error;
                                socket_io_instance->on_io_error_context = on_io_error_context;

                                socket_io_instance->io_state = IO_STATE_OPEN;

                                result = 0;
                            }
                        }
                    }
                    freeaddrinfo(addrInfo);
                }
            }
        }
    }

    if (on_io_open_complete != NULL)
    {
        open_result_detailed.result = result == 0 ? IO_OPEN_OK : IO_OPEN_ERROR;
        on_io_open_complete(on_io_open_complete_context, open_result_detailed);
    }

    return result;
}

int socketio_close(CONCRETE_IO_HANDLE socket_io, ON_IO_CLOSE_COMPLETE on_io_close_complete, void* callback_context)
{
    int result = 0;

    if (socket_io == NULL)
    {
        result = __FAILURE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        if ((socket_io_instance->io_state != IO_STATE_CLOSED) && (socket_io_instance->io_state != IO_STATE_CLOSING))
        {
            // Only close if the socket isn't already in the closed or closing state
            (void)shutdown(socket_io_instance->socket, SHUT_RDWR);
            close(socket_io_instance->socket);
            socket_io_instance->socket = INVALID_SOCKET;
            socket_io_instance->io_state = IO_STATE_CLOSED;
        }

        if (on_io_close_complete != NULL)
        {
            on_io_close_complete(callback_context);
        }

        result = 0;
    }

    return result;
}

int socketio_send(CONCRETE_IO_HANDLE socket_io, const void* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result;

    if ((socket_io == NULL) ||
        (buffer == NULL) ||
        (size == 0))
    {
        /* Invalid arguments */
        LogError("Invalid argument: send given invalid parameter");
        result = __FAILURE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        if (socket_io_instance->io_state != IO_STATE_OPEN)
        {
            LogError("Failure: socket state is not opened.");
            result = __FAILURE__;
        }
        else
        {
            LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
            if (first_pending_io != NULL)
            {
                if (add_pending_io(socket_io_instance, buffer, size, on_send_complete, callback_context) != 0)
                {
                    LogError("Failure: add_pending_io failed.");
                    result = __FAILURE__;
                }
                else
                {
                    result = 0;
                }
            }
            else
            {
                signal(SIGPIPE, SIG_IGN);

                ssize_t send_result = send(socket_io_instance->socket, buffer, size, 0);
                if (send_result != size)
                {
                    if (send_result == INVALID_SOCKET)
                    {
                        if (errno == EAGAIN) /*send says "come back later" with EAGAIN - likely the socket buffer cannot accept more data*/
                        {
                            /*do nothing*/
                            result = 0;
                        }
                        else
                        {
                            LogError("Failure: sending socket failed. errno=%d (%s).", errno, strerror(errno));
                            result = __FAILURE__;
                        }
                    }
                    else
                    {
                        /* queue data */
                        if (add_pending_io(socket_io_instance, buffer + send_result, size - send_result, on_send_complete, callback_context) != 0)
                        {
                            LogError("Failure: add_pending_io failed.");
                            result = __FAILURE__;
                        }
                        else
                        {
                            result = 0;
                        }
                    }
                }
                else
                {
                    if (on_send_complete != NULL)
                    {
                        on_send_complete(callback_context, IO_SEND_OK);
                    }

                    result = 0;
                }
            }
        }
    }

    return result;
}

void socketio_dowork(CONCRETE_IO_HANDLE socket_io)
{
    if (socket_io != NULL)
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        while (first_pending_io != NULL)
        {
            PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)singlylinkedlist_item_get_value(first_pending_io);
            if (pending_socket_io == NULL)
            {
                socket_io_instance->io_state = IO_STATE_ERROR;
                indicate_error(socket_io_instance);
                LogError("Failure: retrieving socket from list");
                break;
            }

            signal(SIGPIPE, SIG_IGN);

            ssize_t send_result = send(socket_io_instance->socket, pending_socket_io->bytes, pending_socket_io->size, 0);
            if (send_result != pending_socket_io->size)
            {
                if (send_result == INVALID_SOCKET)
                {
                    if (errno == EAGAIN) /*send says "come back later" with EAGAIN - likely the socket buffer cannot accept more data*/
                    {
                        /*do nothing until next dowork */
                        break;
                    }
                    else
                    {
                        free(pending_socket_io->bytes);
                        free(pending_socket_io);
                        (void)singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io);

                        LogError("Failure: sending Socket information. errno=%d (%s).", errno, strerror(errno));
                        socket_io_instance->io_state = IO_STATE_ERROR;
                        indicate_error(socket_io_instance);
                    }
                }
                else
                {
                    /* simply wait until next dowork */
                    (void)memmove(pending_socket_io->bytes, pending_socket_io->bytes + send_result, pending_socket_io->size - send_result);
                    pending_socket_io->size -= send_result;
                    break;
                }
            }
            else
            {
                if (pending_socket_io->on_send_complete != NULL)
                {
                    pending_socket_io->on_send_complete(pending_socket_io->callback_context, IO_SEND_OK);
                }

                free(pending_socket_io->bytes);
                free(pending_socket_io);
                if (singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io) != 0)
                {
                    socket_io_instance->io_state = IO_STATE_ERROR;
                    indicate_error(socket_io_instance);
                    LogError("Failure: unable to remove socket from list");
                }
            }

            first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        }

        if (socket_io_instance->io_state == IO_STATE_OPEN)
        {
            ssize_t received = 0;
            do
            {
                received = recv(socket_io_instance->socket, socket_io_instance->recv_bytes, RECEIVE_BYTES_VALUE, 0);
                if (received > 0)
                {
                    if (socket_io_instance->on_bytes_received != NULL)
                    {
                        /* Explicitly ignoring here the result of the callback */
                        (void)socket_io_instance->on_bytes_received(socket_io_instance->on_bytes_received_context, socket_io_instance->recv_bytes, received);
                    }
                }
                else if (received == 0)
                {
                    // Do not log error here due to this is probably the socket being closed on the other end
                    indicate_error(socket_io_instance);
                }
                else if (received < 0 && errno != EAGAIN)
                {
                    LogError("Socketio_Failure: Receiving data from endpoint: errno=%d.", errno);
                    indicate_error(socket_io_instance);
                }

            } while (received > 0 && socket_io_instance->io_state == IO_STATE_OPEN);
        }
    }
}

// Edison is missing this from netinet/tcp.h, but this code still works if we manually define it.
#ifndef SOL_TCP
#define SOL_TCP 6
#endif

#ifndef __APPLE__
static void strtoup(char* str)
{
    if (str != NULL)
    {
        while (*str != '\0')
        {
            if (isalpha((int)*str) && islower((int)*str))
            {
                *str = (char)toupper((int)*str);
            }
            str++;
        }
    }
}
#endif // __APPLE__

int socketio_setoption(CONCRETE_IO_HANDLE socket_io, const char* optionName, const void* value)
{
    int result;

    if (socket_io == NULL ||
        optionName == NULL ||
        value == NULL)
    {
        result = __FAILURE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;

        if (strcmp(optionName, "tcp_keepalive") == 0)
        {
            result = setsockopt(socket_io_instance->socket, SOL_SOCKET, SO_KEEPALIVE, value, sizeof(int));
            if (result == -1) result = errno;
        }
        else if (strcmp(optionName, "tcp_keepalive_time") == 0)
        {
#ifdef __APPLE__
            result = setsockopt(socket_io_instance->socket, IPPROTO_TCP, TCP_KEEPALIVE, value, sizeof(int));
#else
            result = setsockopt(socket_io_instance->socket, SOL_TCP, TCP_KEEPIDLE, value, sizeof(int));
#endif
            if (result == -1) result = errno;
        }
        else if (strcmp(optionName, "tcp_keepalive_interval") == 0)
        {
            result = setsockopt(socket_io_instance->socket, SOL_TCP, TCP_KEEPINTVL, value, sizeof(int));
            if (result == -1) result = errno;
        }
        else if (strcmp(optionName, OPTION_NET_INT_MAC_ADDRESS) == 0)
        {
#ifdef __APPLE__
            LogError("option not supported.");
            result = __FAILURE__;
#else
            if (strlen(value) == 0)
            {
                LogError("option value must be a valid mac address");
                result = __FAILURE__;
            }
            else if ((socket_io_instance->target_mac_address = (char*)malloc(sizeof(char) * (strlen(value) + 1))) == NULL)
            {
                LogError("failed setting net_interface_mac_address option (malloc failed)");
                result = __FAILURE__;
            }
            else if (strcpy(socket_io_instance->target_mac_address, value) == NULL)
            {
                LogError("failed setting net_interface_mac_address option (strcpy failed)");
                free(socket_io_instance->target_mac_address);
                socket_io_instance->target_mac_address = NULL;
                result = __FAILURE__;
            }
            else
            {
                strtoup(socket_io_instance->target_mac_address);
                result = 0;
            }
#endif
        }
        else
        {
            result = __FAILURE__;
        }
    }

    return result;
}

const IO_INTERFACE_DESCRIPTION* socketio_get_interface_description(void)
{
    return &socket_io_interface_description;
}

