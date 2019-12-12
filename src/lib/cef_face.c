/*
 * Copyright (c) 2016, National Institute of Information and Communications
 * Technology (NICT). All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the NICT nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NICT AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE NICT OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * cef_face.c
 */

#define __CEF_FACE_SOURECE__

/****************************************************************************************
 Include Files
 ****************************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <sys/ioctl.h>

#include <cefore/cef_hash.h>
#include <cefore/cef_face.h>
#include <cefore/cef_frame.h>
#include <cefore/cef_log.h>
#include <cefore/cef_client.h>


/****************************************************************************************
 Macros
 ****************************************************************************************/

#define CefC_Face_Type_Num			4
#define CefC_Face_Type_Invalid		0x00
#define CefC_Face_Type_Tcp			0x01
#define CefC_Face_Type_Udp			0x02
#define CefC_Face_Type_Local		0x03

/****************************************************************************************
 Structures Declaration
 ****************************************************************************************/

/****** Entry of Socket Table 			*****/
typedef struct {
	struct sockaddr* ai_addr;
	socklen_t ai_addrlen;
	int 	sock;								/* File descriptor 						*/
	int 	faceid;								/* Assigned Face-ID 					*/
	uint8_t protocol;
} CefT_Sock;

/****************************************************************************************
 State Variables
 ****************************************************************************************/
static CefT_Hash_Handle sock_tbl;				/* Socket Table							*/
static CefT_Face* face_tbl = NULL;				/* Face Table							*/
static uint16_t max_tbl_size = 0;				/* Maximum size of the Tables			*/
static uint16_t process_port_num = 0;			/* The port number that cefnetd uses	*/
static uint16_t assigned_faceid = CefC_Face_Reserved;
												/* Face-ID to assign next				*/
static int doing_ip_version = AF_INET;			/* Version of IP that cefnetd uses		*/
#ifndef CefC_Android
static char local_sock_path[1024];
static int local_sock_path_len = 0;;
#endif // CefC_Android

/****************************************************************************************
 Static Function Declaration
 ****************************************************************************************/

/*--------------------------------------------------------------------------------------
	Deallocates the specified addrinfo
----------------------------------------------------------------------------------------*/
static void
cef_face_addrinfo_free (
	struct addrinfo* ai						/* addrinfo to free 						*/
);
/*--------------------------------------------------------------------------------------
	Creates a new entry of Socket Table
----------------------------------------------------------------------------------------*/
static CefT_Sock*							/* the created new entry					*/
cef_face_sock_entry_create (
	int sock, 								/* file descriptor to register				*/
	struct sockaddr* ai_addr,
	socklen_t ai_addrlen
);
/*--------------------------------------------------------------------------------------
	Destroy the specified entry of Socket Table
----------------------------------------------------------------------------------------*/
static void
cef_face_sock_entry_destroy (
	CefT_Sock* entry						/* the entry to destroy						*/
);
/*--------------------------------------------------------------------------------------
	Looks up Face-ID that is not used
----------------------------------------------------------------------------------------*/
static int									/* Face-ID that is not used					*/
cef_face_unused_faceid_search (
	void
);
/*--------------------------------------------------------------------------------------
	Looks up and creates the specified Face
----------------------------------------------------------------------------------------*/
static int									/* Face-ID									*/
cef_face_lookup_faceid (
	const char* destination, 				/* String of the destination address 		*/
	int protocol,							/* protoco (udp,tcp,local) 					*/
	int* create_f							/* set 1 if this face is new 				*/
);

#ifdef CefC_DebugOld
/****************************************************************************************
 For Debug Trace
 ****************************************************************************************/

extern unsigned int CEF_DEBUG;

#endif // CefC_DebugOld

/****************************************************************************************
 ****************************************************************************************/

/*--------------------------------------------------------------------------------------
	Initialize the face module
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_init (
	uint8_t 	node_type					/* Node Type (Router/Receiver....)			*/
){
	
	/* Creates the Socket Table and Face Table 		*/
	if (face_tbl != NULL) {
		cef_log_write (CefC_Log_Error, "%s (face_tbl)\n", __func__);
		return (-1);
	}
	switch (node_type) {
		case CefC_Node_Type_Receiver: {
			max_tbl_size = CefC_Face_Receiver_Max;
			break;
		}
		case CefC_Node_Type_Publisher: {
			max_tbl_size = CefC_Face_Publisher_Max;
			break;
		}
		case CefC_Node_Type_Router: {
			max_tbl_size = CefC_Face_Router_Max;
			break;
		}
		default: {
			/* NOP */;
			break;
		}
	}
	if (max_tbl_size == 0) {
		cef_log_write (CefC_Log_Error, "%s (max_tbl_size)\n", __func__);
		return (-1);
	}
	
	face_tbl = (CefT_Face*) malloc (sizeof (CefT_Face) * max_tbl_size);
	memset (face_tbl, 0, sizeof (CefT_Face) * max_tbl_size);
	sock_tbl = cef_hash_tbl_create ((uint16_t) max_tbl_size);
	
#ifndef CefC_Android
	local_sock_path_len = cef_client_local_sock_name_get (local_sock_path);
#endif // CefC_Android
	
	return (1);
}
/*--------------------------------------------------------------------------------------
	Looks up and creates the Face from the specified string of destination address
----------------------------------------------------------------------------------------*/
int											/* Face-ID									*/
cef_face_lookup_faceid_from_addrstr (
	const char* destination,				/* String of destination address 			*/
	const char* protocol					/* protoco (udp,tcp,local) 					*/
) {
	int faceid;
	int create_f = 0;
	int msg_len;
	unsigned char buff[CefC_Max_Length];
	int prot_index = CefC_Face_Type_Invalid;
	
	if (strcmp (protocol, "udp") == 0) {
		prot_index = CefC_Face_Type_Udp;
	}
	if (strcmp (protocol, "tcp") == 0) {
		prot_index = CefC_Face_Type_Tcp;
	}
	
	faceid = cef_face_lookup_faceid (destination, prot_index, &create_f);
	
	if ((faceid > 0) && (create_f)) {
		/* send a link message */
		msg_len = cef_frame_interest_link_msg_create (buff);
		
		if (msg_len > 0) {
#ifdef CefC_Debug
			cef_dbg_write (CefC_Dbg_Finer, 
				"Send a Interest Link message to FID#%d\n", faceid);
#endif // CefC_Debug
			cef_face_frame_send_forced (faceid, buff, (size_t) msg_len);
		}
	}
	
	return (faceid);
}
/*--------------------------------------------------------------------------------------
	Searches the specified Face
----------------------------------------------------------------------------------------*/
int											/* Face-ID									*/
cef_face_search_faceid (
	const char* destination, 				/* String of the destination address 		*/
	const char* protocol					/* protoco (udp,tcp,local) 					*/
) {
	int prot_index = CefC_Face_Type_Invalid;
	char peer[512];
	CefT_Sock* entry;
	
	if (strcmp (protocol, "udp") == 0) {
		prot_index = CefC_Face_Type_Udp;
	}
	if (strcmp (protocol, "tcp") == 0) {
		prot_index = CefC_Face_Type_Tcp;
	}
	sprintf (peer, "%s:%d", destination, prot_index);
	
	entry = (CefT_Sock*) cef_hash_tbl_item_get (
				sock_tbl, (const unsigned char*) peer, strlen (peer));
	if (entry) {
		return (entry->faceid);
	}
	
	return (-1);
}
/*--------------------------------------------------------------------------------------
	Updates the listen faces with TCP
----------------------------------------------------------------------------------------*/
int											/* number of the listen face with TCP 		*/
cef_face_update_tcp_faces (
	struct pollfd* intcpfds,
	uint16_t* intcpfaces,
	uint8_t intcpfdc
) {
	int i, n;
	int add_f;
	int new_intcpfdc = intcpfdc;

	for (n = CefC_Face_Reserved ; n < assigned_faceid ; n++) {
		if (face_tbl[n].protocol != CefC_Face_Type_Tcp) {
			continue;
		}
		add_f = 1;
		for (i = 0 ; i < intcpfdc ; i++) {
			if (face_tbl[n].fd == intcpfds[i].fd) {
				add_f = 0;
				break;
			}
		}
		if (add_f) {
			intcpfaces[new_intcpfdc] = n;
			intcpfds[new_intcpfdc].fd = face_tbl[n].fd;
			intcpfds[new_intcpfdc].events = POLLIN | POLLERR;
			new_intcpfdc++;
		}
	}

	return (new_intcpfdc);
}
/*--------------------------------------------------------------------------------------
	Looks up and creates the peer Face
----------------------------------------------------------------------------------------*/
int											/* Peer Face-ID 							*/
cef_face_lookup_peer_faceid (
	struct addrinfo* sas, 					/* sockaddr_storage structure				*/
	socklen_t sas_len,						/* length of sockaddr_storage				*/
	int protocol
) {
	char 	name[NI_MAXHOST];
	int 	result;
	CefT_Sock* entry;
	int 	faceid;
	char 	peer[512];
	
	/* Obtains the source node's information 	*/
	result = getnameinfo ((struct sockaddr*) sas, sas_len, 
				name, sizeof (name), 0, 0, NI_NUMERICHOST);

	if (result != 0) {
		cef_log_write (CefC_Log_Error, "%s (getnameinfo:%d)\n", __func__, result);
		return (-1);
	}
	
	/* Looks up the source node's information from the source table 	*/
	sprintf (peer, "%s:%d", name, protocol);
	entry = (CefT_Sock*) cef_hash_tbl_item_get (
									sock_tbl,
									(const unsigned char*) peer, strlen (peer));
	if (entry) {
#ifdef CefC_Debug
		cef_dbg_write (CefC_Dbg_Finest, 
			"[face] Lookup the Face#%d for %s\n", entry->faceid, peer);
#endif // CefC_Debug
		return (entry->faceid);
	}
	
	faceid = cef_face_lookup_faceid (name, protocol, NULL);
	
#ifdef CefC_Debug
	cef_dbg_write (CefC_Dbg_Finer, 
		"[face] Creation the new Face#%d for %s\n", faceid, peer);
#endif // CefC_Debug
	
	return (faceid);
}
/*--------------------------------------------------------------------------------------
	Searches and creates the local Face-ID corresponding to the specified FD
----------------------------------------------------------------------------------------*/
int												/* the corresponding Face-ID			*/
cef_face_lookup_local_faceid (
	int fd										/* File descriptor						*/
) {
	char 	name[1024];
	int 	faceid;
	int 	index;
	CefT_Sock* entry;

	/* Creates the name for the local socket 	*/
	sprintf (name, "app-face-%d", fd);

	/* Looks up the source node's information from the source table 	*/
	entry = (CefT_Sock*) cef_hash_tbl_item_get (
									sock_tbl, (const unsigned char*)name, strlen (name));
	if (entry) {
#ifdef CefC_Debug
		cef_dbg_write (CefC_Dbg_Finer, 
			"[face] Lookup the Face#%d (FD#%d) for local peer\n", entry->faceid, fd);
#endif // CefC_Debug
		/* Finds and returns an existing entry 	*/
		return (entry->faceid);
	}

	/* Looks up Face-ID that is not used 		*/
	faceid = cef_face_unused_faceid_search ();
	if (faceid < 0) {
		return (-1);
	}

	/* Creates a new entry of Socket Table 		*/
	entry = cef_face_sock_entry_create (fd, NULL, 0);
	entry->faceid = faceid;

	/* Sets the created entry into Socket Table	*/
	index = cef_hash_tbl_item_set (
							sock_tbl, (const unsigned char*)name, strlen (name), entry);

	if (index < 0) {
		cef_face_sock_entry_destroy (entry);
		return (-1);
	}

	/* Registers the created entry into Face Table	*/
	face_tbl[faceid].index = index;
	face_tbl[faceid].fd = entry->sock;
	face_tbl[faceid].local_f = 1;
	
#ifdef CefC_Debug
	cef_dbg_write (CefC_Dbg_Finer, 
		"[face] Creation the new Face#%d (FD#%d) for local peer\n", entry->faceid, fd);
#endif // CefC_Debug
	
	return (faceid);
}
/*--------------------------------------------------------------------------------------
	Closes the specified Face
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_close (
	int faceid								/* Face-ID									*/
) {
	CefT_Sock* entry;
	
	entry = (CefT_Sock*) cef_hash_tbl_item_remove_from_index (
										sock_tbl, face_tbl[faceid].index);
	
	if (entry) {
#ifdef CefC_Debug
		cef_dbg_write (CefC_Dbg_Finer, 
			"[face] Close the Face#%d (FD#%d)\n", faceid, face_tbl[entry->faceid].fd);
#endif // CefC_Debug
		face_tbl[faceid].index = 0;
		face_tbl[faceid].fd = 0;
		close (entry->sock);
		free (entry);
	}

	return (1);
}
/*--------------------------------------------------------------------------------------
	Checks the specified Face is active or not
----------------------------------------------------------------------------------------*/
int										/* Returns the value less than 1 if it fails 	*/
cef_face_check_active (
	int faceid								/* Face-ID									*/
) {
	return (face_tbl[faceid].fd);
}
/*--------------------------------------------------------------------------------------
	Creates the listening UDP socket with the specified port
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_udp_listen_face_create (
	uint16_t 		port_num				/* Port Number that cefnetd listens			*/
) {
	struct addrinfo hints;
	struct addrinfo* res;
	struct addrinfo* cres;
	int err;
	char port_str[32];
	int sock;
	CefT_Sock* entryv4 = NULL;
	CefT_Sock* entryv6 = NULL;
	char ip_str[64];
	char if_str[128];
	int indexv4 = -1;
	int indexv6 = -1;

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;

	process_port_num = port_num;

	sprintf (port_str, "%d", port_num);

	if ((err = getaddrinfo (NULL, port_str, &hints, &res)) != 0) {
		cef_log_write (CefC_Log_Error, 
			"%s (getaddrinfo:%s)\n", __func__, gai_strerror(err));
		return (-1);
	}

	for (cres = res ; cres != NULL ; cres = res) {
		res = cres->ai_next;

		sock = socket (cres->ai_family, cres->ai_socktype, 0);
		if (sock < 0) {
			continue;
		}

		memset (ip_str, 0, 64);
		if (getnameinfo (cres->ai_addr, (int)cres->ai_addrlen
				, ip_str, sizeof (ip_str), 0, 0,  NI_NUMERICHOST) != 0) {
			continue;
		}
		sprintf (if_str, "%s:udp", ip_str);

		switch (cres->ai_family) {
			case AF_INET: {
				if (indexv4 >= 0) {
					cef_face_addrinfo_free (cres);
					close (sock);
				} else {
					cres->ai_next = NULL;
					entryv4 = cef_face_sock_entry_create (
								sock, cres->ai_addr, cres->ai_addrlen);
					entryv4->faceid = CefC_Faceid_ListenUdpv4;
					entryv4->protocol = CefC_Face_Type_Udp;
					indexv4 = cef_hash_tbl_item_set (
						sock_tbl, (const unsigned char*)if_str, strlen (if_str), entryv4);
				}
				break;
			}
			case AF_INET6: {
				if (indexv6 >= 0) {
					cef_face_addrinfo_free (cres);
					close (sock);
				} else {
					cres->ai_next = NULL;
					entryv6 = cef_face_sock_entry_create (
								sock, cres->ai_addr, cres->ai_addrlen);
					entryv6->faceid = CefC_Faceid_ListenUdpv6;
					entryv6->protocol = CefC_Face_Type_Udp;
					indexv6 = cef_hash_tbl_item_set (
						sock_tbl, (const unsigned char*)if_str, strlen (if_str), entryv6);
				}
				break;
			}
			default: {
				/* NOP */;
				break;
			}
		}
	}

	if (indexv4 >= 0) {
		if (bind (entryv4->sock, entryv4->ai_addr, entryv4->ai_addrlen) < 0) {
			close (entryv4->sock);
			face_tbl[CefC_Faceid_ListenUdpv4].index = indexv4;
			face_tbl[CefC_Faceid_ListenUdpv4].fd = 0;
			cef_log_write (CefC_Log_Error, 
				"[face] Failed to create the listen face with UDP\n");
			return (-1);
		} else {
			face_tbl[CefC_Faceid_ListenUdpv4].index = indexv4;
			face_tbl[CefC_Faceid_ListenUdpv4].fd = entryv4->sock;
			face_tbl[CefC_Faceid_ListenUdpv4].protocol = CefC_Face_Type_Udp;
			return (CefC_Faceid_ListenUdpv4);
		}
	}

	if (indexv6 >= 0) {
		if (bind (entryv6->sock, entryv6->ai_addr, entryv6->ai_addrlen) < 0) {
			close (entryv6->sock);
			face_tbl[CefC_Faceid_ListenUdpv6].index = indexv6;
			face_tbl[CefC_Faceid_ListenUdpv6].fd = 0;
		} else {
			doing_ip_version = AF_INET6;
			face_tbl[CefC_Faceid_ListenUdpv6].index = indexv6;
			face_tbl[CefC_Faceid_ListenUdpv6].fd = entryv6->sock;
			face_tbl[CefC_Faceid_ListenUdpv6].protocol = CefC_Face_Type_Udp;
			return (CefC_Faceid_ListenUdpv6);
		}
	}
	cef_log_write (CefC_Log_Error, "[face] Failed to create the listen face with UDP\n");
	
	return (-1);
}
/*--------------------------------------------------------------------------------------
	Creates the listening TCP socket with the specified port
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_tcp_listen_face_create (
	uint16_t 		port_num				/* Port Number that cefnetd listens			*/
) {
	struct addrinfo hints;
	struct addrinfo* res;
	struct addrinfo* cres;
	int err;
	char port_str[32];
	int sock;
	CefT_Sock* entryv4 = NULL;
	CefT_Sock* entryv6 = NULL;
	char ip_str[64];
	char if_str[128];
	int indexv4 = -1;
	int indexv6 = -1;
	int reuse_f = 1;
	int flag;

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	process_port_num = port_num;

	sprintf (port_str, "%d", port_num);

	if ((err = getaddrinfo (NULL, port_str, &hints, &res)) != 0) {
		cef_log_write (CefC_Log_Error, 
			"%s (getaddrinfo:%s)\n", __func__, gai_strerror(err));
		return (-1);
	}

	for (cres = res ; cres != NULL ; cres = res) {
		res = cres->ai_next;

		sock = socket (cres->ai_family, cres->ai_socktype, 0);
		if (sock < 0) {
			cef_log_write (CefC_Log_Error, "%s (socket:%s)\n", __func__, strerror(errno));
			continue;
		}
		setsockopt (sock,
			SOL_SOCKET, SO_REUSEADDR, &reuse_f, sizeof (reuse_f));

		memset (ip_str, 0, 64);
		if (getnameinfo (cres->ai_addr, (int)cres->ai_addrlen
				, ip_str, sizeof (ip_str), 0, 0,  NI_NUMERICHOST) != 0) {
			continue;
		}
		sprintf (if_str, "%s:tcp", ip_str);

		switch (cres->ai_family) {
			case AF_INET: {
				if (indexv4 >= 0) {
					cef_face_addrinfo_free (cres);
					close (sock);
				} else {
					cres->ai_next = NULL;
					entryv4 = cef_face_sock_entry_create (
									sock, cres->ai_addr, cres->ai_addrlen);
					entryv4->faceid = CefC_Faceid_ListenTcpv4;
					entryv4->protocol = CefC_Face_Type_Tcp;
					indexv4 = cef_hash_tbl_item_set (
						sock_tbl, (const unsigned char*)if_str, strlen (if_str), entryv4);
				}
				break;
			}
			case AF_INET6: {
				if (indexv6 >= 0) {
					cef_face_addrinfo_free (cres);
					close (sock);
				} else {
					cres->ai_next = NULL;
					entryv6 = cef_face_sock_entry_create (
									sock, cres->ai_addr, cres->ai_addrlen);
					entryv6->faceid = CefC_Faceid_ListenTcpv6;
					entryv6->protocol = CefC_Face_Type_Tcp;
					indexv6 = cef_hash_tbl_item_set (
						sock_tbl, (const unsigned char*)if_str, strlen (if_str), entryv6);
				}
				break;
			}
			default: {
				/* NOP */;
				break;
			}
		}
	}

	if (indexv4 >= 0) {
		if (bind (entryv4->sock, entryv4->ai_addr, entryv4->ai_addrlen) < 0) {
			close (entryv4->sock);
			face_tbl[CefC_Faceid_ListenTcpv4].index = indexv4;
			face_tbl[CefC_Faceid_ListenTcpv4].fd = 0;
			cef_log_write (CefC_Log_Error, 
				"[face] Failed to create the listen face with TCP\n");
			return (-1);
		} else {
			if (listen (entryv4->sock, 16) < 0) {
				cef_log_write (CefC_Log_Error, 
					"%s (listen:%s)\n", __func__, strerror(errno));
				return (-1);
			}
			flag = fcntl (entryv4->sock, F_GETFL, 0);
			if (flag < 0) {
				cef_log_write (CefC_Log_Error, 
					"%s (fcntl:%s)\n", __func__, strerror(errno));
				return (-1);
			}
			if (fcntl (entryv4->sock, F_SETFL, flag | O_NONBLOCK) < 0) {
				cef_log_write (CefC_Log_Error, 
					"%s (fcntl:%s)\n", __func__, strerror(errno));
				return (-1);
			}
			face_tbl[CefC_Faceid_ListenTcpv4].index = indexv4;
			face_tbl[CefC_Faceid_ListenTcpv4].fd = entryv4->sock;
			face_tbl[CefC_Faceid_ListenTcpv4].protocol = CefC_Face_Type_Tcp;
			return (CefC_Faceid_ListenTcpv4);
		}
	}

	if (indexv6 >= 0) {
		if (bind (entryv6->sock, entryv6->ai_addr, entryv6->ai_addrlen) < 0) {
			close (entryv6->sock);
			face_tbl[CefC_Faceid_ListenTcpv6].index = indexv6;
			face_tbl[CefC_Faceid_ListenTcpv6].fd = 0;
		} else {
			if (listen (entryv6->sock, 16) < 0) {
				cef_log_write (CefC_Log_Error, 
					"%s (listen/v6:%s)\n", __func__, strerror(errno));
				return (-1);
			}
			flag = fcntl (entryv6->sock, F_GETFL, 0);
			if (flag < 0) {
				cef_log_write (CefC_Log_Error, 
					"%s (fcntl/v6:%s)\n", __func__, strerror(errno));
				return (-1);
			}
			if (fcntl (entryv6->sock, F_SETFL, flag | O_NONBLOCK) < 0) {
				cef_log_write (CefC_Log_Error, 
					"%s (fcntl/v6:%s)\n", __func__, strerror(errno));
				return (-1);
			}
			doing_ip_version = AF_INET6;
			face_tbl[CefC_Faceid_ListenTcpv6].index = indexv6;
			face_tbl[CefC_Faceid_ListenTcpv6].fd = entryv6->sock;
			face_tbl[CefC_Faceid_ListenTcpv6].protocol = CefC_Face_Type_Tcp;
			return (CefC_Faceid_ListenTcpv6);
		}
	}

	cef_log_write (CefC_Log_Error, "[face] Failed to create the listen face with TCP\n");

	return (-1);
}
/*--------------------------------------------------------------------------------------
	Creates the listening UDP socket for NDN with the specified port
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_ndn_listen_face_create (
	uint16_t 		port_num				/* Port Number that cefnetd listens			*/
) {
	struct addrinfo hints;
	struct addrinfo* res;
	struct addrinfo* cres;
	int err;
	char port_str[32];
	int sock;
	CefT_Sock* entryv4;
	CefT_Sock* entryv6;
	char ip_str[64];
	char if_str[128];
	int indexv4 = -1;
	int indexv6 = -1;

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;
	
	sprintf (port_str, "%d", port_num);

	if ((err = getaddrinfo (NULL, port_str, &hints, &res)) != 0) {
		cef_log_write (CefC_Log_Error, 
			"%s (getaddrinfo:%s)\n", __func__, gai_strerror(err));
		return (-1);
	}

	for (cres = res ; cres != NULL ; cres = res) {
		res = cres->ai_next;

		sock = socket (cres->ai_family, cres->ai_socktype, 0);
		if (sock < 0) {
			continue;
		}

		memset (ip_str, 0, 64);
		if (getnameinfo (cres->ai_addr, (int)cres->ai_addrlen
				, ip_str, sizeof (ip_str), 0, 0,  NI_NUMERICHOST) != 0) {
			continue;
		}
		sprintf (if_str, "%s:ndn", ip_str);
		
		switch (cres->ai_family) {
			case AF_INET: {
				if (indexv4 >= 0) {
					cef_face_addrinfo_free (cres);
					close (sock);
				} else {
					cres->ai_next = NULL;
					entryv4 = cef_face_sock_entry_create (
								sock, cres->ai_addr, cres->ai_addrlen);
					entryv4->faceid = CefC_Faceid_ListenNdnv4;
					entryv4->protocol = CefC_Face_Type_Udp;
					indexv4 = cef_hash_tbl_item_set (
						sock_tbl, (const unsigned char*)if_str, strlen (if_str), entryv4);
				}
				break;
			}
			case AF_INET6: {
				if (indexv6 >= 0) {
					cef_face_addrinfo_free (cres);
					close (sock);
				} else {
					cres->ai_next = NULL;
					entryv6 = cef_face_sock_entry_create (
								sock, cres->ai_addr, cres->ai_addrlen);
					entryv6->faceid = CefC_Faceid_ListenNdnv6;
					entryv6->protocol = CefC_Face_Type_Udp;
					indexv6 = cef_hash_tbl_item_set (
						sock_tbl, (const unsigned char*)if_str, strlen (if_str), entryv6);
				}
				break;
			}
			default: {
				/* NOP */;
				break;
			}
		}
	}
	
	if (indexv4 >= 0) {
		if (bind (entryv4->sock, entryv4->ai_addr, entryv4->ai_addrlen) < 0) {
			close (entryv4->sock);
			face_tbl[CefC_Faceid_ListenNdnv4].index = indexv4;
			face_tbl[CefC_Faceid_ListenNdnv4].fd = 0;
		} else {
			face_tbl[CefC_Faceid_ListenNdnv4].index = indexv4;
			face_tbl[CefC_Faceid_ListenNdnv4].fd = entryv4->sock;
			face_tbl[CefC_Faceid_ListenNdnv4].protocol = CefC_Face_Type_Udp;
			return (CefC_Faceid_ListenNdnv4);
		}
	}
	
	if (indexv6 >= 0) {
		if (bind (entryv6->sock, entryv6->ai_addr, entryv6->ai_addrlen) < 0) {
			close (entryv6->sock);
			face_tbl[CefC_Faceid_ListenNdnv6].index = indexv6;
			face_tbl[CefC_Faceid_ListenNdnv6].fd = 0;
		} else {
			face_tbl[CefC_Faceid_ListenNdnv6].index = indexv6;
			face_tbl[CefC_Faceid_ListenNdnv6].fd = entryv6->sock;
			face_tbl[CefC_Faceid_ListenNdnv6].protocol = CefC_Face_Type_Udp;
			return (CefC_Faceid_ListenNdnv6);
		}
	}

	cef_log_write (CefC_Log_Error, "[face] Failed to create the listen face for NFD\n");

	return (-1);
}
/*--------------------------------------------------------------------------------------
	Accepts the TCP socket
----------------------------------------------------------------------------------------*/
int													/* Face-ID 							*/
cef_face_accept_connect (
	void
) {
	struct sockaddr_storage* sa;
	socklen_t len = sizeof (struct sockaddr_storage);
	int cs;
	int flag;
	CefT_Sock* entry;
	int faceid;
	int index;
	char ip_str[256];
	char port_str[256];
	char peer_str[256];
	int msg_len;
	unsigned char buff[CefC_Max_Length];
	
	sa = (struct sockaddr_storage*) malloc (sizeof (struct sockaddr_storage));
	memset (sa, 0, sizeof (struct sockaddr_storage));
	cs = accept (face_tbl[CefC_Faceid_ListenTcpv4].fd, (struct sockaddr*) sa, &len);
	if (cs < 0) {
		cs = accept (face_tbl[CefC_Faceid_ListenTcpv6].fd, (struct sockaddr*) sa, &len);
		if (cs < 0) {
			free (sa);
			return (-1);
		}
	}
	
	flag = fcntl (cs, F_GETFL, 0);
	if (flag < 0) {
		goto POST_ACCEPT;
	}
	if (fcntl (cs, F_SETFL, flag | O_NONBLOCK) < 0) {
		goto POST_ACCEPT;
	}

	if (getnameinfo ((struct sockaddr*) sa, len, ip_str, sizeof (ip_str), 
			port_str, sizeof (port_str),  NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
		goto POST_ACCEPT;
	}
	
	/* Looks up the source node's information from the source table 	*/
	sprintf (peer_str, "%s:%d", ip_str, CefC_Face_Type_Tcp);
	entry = (CefT_Sock*) cef_hash_tbl_item_get (
							sock_tbl, (const unsigned char*) peer_str, strlen (peer_str));
	
	if (entry) {
		cef_face_close (entry->faceid);
	}
	faceid = cef_face_unused_faceid_search ();
	if (faceid < 0) {
		goto POST_ACCEPT;
	}
	entry = cef_face_sock_entry_create (cs, (struct sockaddr*) sa, len);
	entry->faceid = faceid;
	entry->protocol = CefC_Face_Type_Tcp;

	index = cef_hash_tbl_item_set (
		sock_tbl, (const unsigned char*) peer_str, strlen (peer_str), entry);

	if (index < 0) {
		cef_face_sock_entry_destroy (entry);
		sa = NULL;
		goto POST_ACCEPT;
	}
	face_tbl[faceid].index = index;
	face_tbl[faceid].fd = entry->sock;
	face_tbl[faceid].protocol = CefC_Face_Type_Tcp;

	/* send a link message */
	msg_len = cef_frame_interest_link_msg_create (buff);
	if (msg_len > 0) {
#ifdef CefC_Debug
		cef_dbg_write (CefC_Dbg_Finer, 
			"Send a Interest Link message to FID#%d\n", entry->faceid);
#endif // CefC_Debug
		cef_face_frame_send_forced (entry->faceid, buff, (size_t) msg_len);
	}
	
	return (faceid);

POST_ACCEPT:
	close (cs);
	if (sa) {
		free (sa);
	}

	return (-1);
}
/*--------------------------------------------------------------------------------------
	Creates the local face that uses UNIX domain socket
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_local_face_create (
	int sk_type
) {
	struct sockaddr_un saddr;
	int flag;
	int sock;
	int index;
	CefT_Sock* entry;
	
	if ((sock = socket (AF_UNIX, sk_type, 0)) < 0) {
		cef_log_write (CefC_Log_Error, "%s (sock:%s)\n", __func__, strerror(errno));
		return (-1);
	}
	
	/* Initialize a sockaddr_un 	*/
	memset (&saddr, 0, sizeof (saddr));
	saddr.sun_family = AF_UNIX;
#ifdef CefC_Android
	/* Android socket Name starts with \0.	*/
	memcpy(saddr.sun_path, CefC_Local_Sock_Name, CefC_Local_Sock_Name_Len);
	
	/* Prepares a source socket 	*/
	unlink (CefC_Local_Sock_Name);
#else // CefC_Android
	strcpy (saddr.sun_path, local_sock_path);
	
	/* Prepares a source socket 	*/
	unlink (local_sock_path);
#endif // CefC_Android
	
	
#ifdef CefC_Android
	if (bind (sock, (struct sockaddr *)&saddr
				, sizeof (saddr.sun_family) + CefC_Local_Sock_Name_Len) < 0) {
		LOGE("%s:%u, ERROR:%s\n", __func__, __LINE__, strerror (errno));
		return (-1);
	}
#else // CefC_Android

#ifdef __APPLE__
	saddr.sun_len = sizeof (saddr);

	if (bind (sock, (struct sockaddr *)&saddr, SUN_LEN (&saddr)) < 0) {
		cef_log_write (CefC_Log_Error, "%s (bind:%s)\n", __func__, strerror(errno));
		return (-1);
	}
#else // __APPLE__
	if (bind (sock, (struct sockaddr *)&saddr
				, sizeof (saddr.sun_family) + local_sock_path_len) < 0) {
		cef_log_write (CefC_Log_Error, "%s (bind:%s)\n", __func__, strerror(errno));
		return (-1);
	}
#endif // __APPLE__

#endif // CefC_Android

	switch ( sk_type ) {
	case SOCK_STREAM:
	case SOCK_SEQPACKET:
		if (listen (sock, 1) < 0) {
			cef_log_write (CefC_Log_Error, "%s (listen:%s)\n", __func__, strerror(errno));
			return (-1);
		}
		break;
	default:
		break;
	}
	flag = fcntl (sock, F_GETFL, 0);
	if (flag < 0) {
		cef_log_write (CefC_Log_Error, "%s (fcntl:%s)\n", __func__, strerror(errno));
		return (-1);
	}
	if (fcntl (sock, F_SETFL, flag | O_NONBLOCK) < 0) {
		cef_log_write (CefC_Log_Error, "%s (fcntl:%s)\n", __func__, strerror(errno));
		return (-1);
	}

	entry = cef_face_sock_entry_create (sock, NULL, 0);
	entry->faceid = CefC_Faceid_Local;
#ifndef CefC_Android
	index = cef_hash_tbl_item_set (
		sock_tbl, 
		(const unsigned char*)local_sock_path, 
		local_sock_path_len, 
		entry);
#else // CefC_Android
	index = cef_hash_tbl_item_set (
		sock_tbl,
		(const unsigned char*)CefC_Local_Sock_Name,
		CefC_Local_Sock_Name_Len,
		entry);
#endif // CefC_Android
	face_tbl[CefC_Faceid_Local].index 	= index;
	face_tbl[CefC_Faceid_Local].fd 		= entry->sock;

	return (CefC_Faceid_Local);
}

/*--------------------------------------------------------------------------------------
	Converts the specified Face-ID into the corresponding file descriptor
----------------------------------------------------------------------------------------*/
int											/* the corresponding file descriptor		*/
cef_face_get_fd_from_faceid (
	uint16_t 		faceid					/* Face-ID									*/
) {
	return (face_tbl[faceid].fd);
}
/*--------------------------------------------------------------------------------------
	Obtains the Face structure from the specified Face-ID
----------------------------------------------------------------------------------------*/
CefT_Face* 									/* Face 									*/
cef_face_get_face_from_faceid (
	uint16_t 	faceid						/* Face-ID									*/
) {
	assert (faceid >= 0 && faceid <= max_tbl_size);
	return (&face_tbl[faceid]);
}
/*--------------------------------------------------------------------------------------
	Obtains the Face structure from the specified Face-ID
----------------------------------------------------------------------------------------*/
uint32_t 
cef_face_get_seqnum_from_faceid (
	uint16_t 	faceid						/* Face-ID									*/
) {
	assert (faceid >= 0 && faceid <= max_tbl_size);
	face_tbl[faceid].seqnum++;
	return (face_tbl[faceid].seqnum);
}

/*--------------------------------------------------------------------------------------
	Sends a message via the specified Face
----------------------------------------------------------------------------------------*/
void
cef_face_frame_send_forced (
	uint16_t 		faceid, 				/* Face-ID indicating the destination 		*/
	unsigned char* 	msg, 					/* a message to send						*/
	size_t			msg_len					/* length of the message to send 			*/
) {
	CefT_Sock* entry;
	int res;
	
	entry = (CefT_Sock*) cef_hash_tbl_item_get_from_index (
										sock_tbl, face_tbl[faceid].index);
	
	if (face_tbl[faceid].local_f) {
		send (entry->sock, msg, msg_len, 0);
	} else {
		if (face_tbl[faceid].protocol != CefC_Face_Type_Tcp) {
			sendto (entry->sock, msg, msg_len
					, 0, entry->ai_addr, entry->ai_addrlen);
		} else {
			res = write (entry->sock, msg, msg_len);
			if (res < 0) {
				cef_face_close (faceid);
			}
		}
	}

	return;
}
/*--------------------------------------------------------------------------------------
	Sends a Content Object via the specified Face
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_object_send (
	uint16_t 		faceid, 				/* Face-ID indicating the destination 		*/
	unsigned char* 	msg, 					/* a message to send						*/
	size_t			msg_len,				/* length of the message to send 			*/
	unsigned char* 	payload, 				/* a message to send						*/
	size_t			payload_len,			/* length of the message to send 			*/
	uint32_t		chnk_num				/* Chunk Number 							*/
) {
	CefT_Sock* entry;
	unsigned char app_frame[CefC_Max_Length];
	struct cef_app_hdr app_hdr;
	int res;

	if (face_tbl[faceid].fd < 3) {
		return (-1);
	}

	entry = (CefT_Sock*) cef_hash_tbl_item_get_from_index (
										sock_tbl, face_tbl[faceid].index);
	if (face_tbl[faceid].local_f) {

		app_hdr.ver 	= CefC_App_Version;
		app_hdr.type 	= CefC_App_Type_Internal;
		app_hdr.len 	= (uint32_t) payload_len;
		app_hdr.chnk_num = chnk_num;

		memcpy (app_frame, &app_hdr, sizeof (struct cef_app_hdr));
		memcpy (app_frame + sizeof (struct cef_app_hdr), payload, payload_len);

		send (entry->sock, app_frame, payload_len + CefC_App_Header_Size, 0);

	} else {
		if (face_tbl[faceid].protocol != CefC_Face_Type_Tcp) {
			sendto (entry->sock, msg, msg_len
					, 0, entry->ai_addr, entry->ai_addrlen);
		} else {
			res = write (entry->sock, msg, msg_len);
			if (res < 0) {
				cef_face_close (faceid);
			}
		}
	}

	return (1);
}
/*--------------------------------------------------------------------------------------
	Sends a Content Object if the specified is local Face
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_object_send_iflocal (
	uint16_t 		faceid, 				/* Face-ID indicating the destination 		*/
	unsigned char* 	payload, 				/* a message to send						*/
	size_t			payload_len,			/* length of the message to send 			*/
	uint32_t		chnk_num				/* Chunk Number 							*/
) {
	CefT_Sock* entry;
	unsigned char app_frame[CefC_Max_Length];
	struct cef_app_hdr app_hdr;
	int ret = 1;

	if (face_tbl[faceid].fd < 3) {
		return (-1);
	}

	entry = (CefT_Sock*) cef_hash_tbl_item_get_from_index (
										sock_tbl, face_tbl[faceid].index);
	if (face_tbl[faceid].local_f) {

		app_hdr.ver 		= CefC_App_Version;
		app_hdr.type 		= CefC_App_Type_Internal;
		app_hdr.len 		= (uint32_t) payload_len;
		app_hdr.chnk_num 	= chnk_num;

		memcpy (app_frame, &app_hdr, sizeof (struct cef_app_hdr));
		memcpy (app_frame + sizeof (struct cef_app_hdr), payload, payload_len);

		send (entry->sock, app_frame, payload_len + CefC_App_Header_Size, 0);

	} else {
		ret = 0;
	}

	return (ret);
}
/*--------------------------------------------------------------------------------------
	Checks whether the specified Face is local or not
----------------------------------------------------------------------------------------*/
int											/* local face is 1, no-local face is 0	 	*/
cef_face_is_local_face (
	uint16_t 		faceid 					/* Face-ID indicating the destination 		*/
) {
	if (face_tbl[faceid].local_f) {
		return (1);
	}

	return (0);
}
/*--------------------------------------------------------------------------------------
	Obtains type of Face (local/UDP/TCP)
----------------------------------------------------------------------------------------*/
int											/* type of Face							 	*/
cef_face_type_get (
	uint16_t 		faceid 					/* Face-ID									*/
) {
	if (face_tbl[faceid].local_f) {
		return (CefC_Face_Type_Local);
	}
	return ((int) face_tbl[faceid].protocol);
}
/*--------------------------------------------------------------------------------------
	Sends a message if the specified is local Face with API Header
----------------------------------------------------------------------------------------*/
int											/* Returns a negative value if it fails 	*/
cef_face_apimsg_send_iflocal (
	uint16_t 		faceid, 				/* Face-ID indicating the destination 		*/
	void *			api_hdr, 				/* a header to send							*/
	size_t			api_hdr_len,			/* length of the header to send 			*/
	void *			payload, 				/* a message to send						*/
	size_t			payload_len				/* length of the message to send 			*/
) {
	CefT_Sock* entry;
	unsigned char api_frame[CefC_Max_Length];
	int ret = 1;

	if (face_tbl[faceid].fd < 3) {
		return (-1);
	}

	entry = (CefT_Sock*) cef_hash_tbl_item_get_from_index (
										sock_tbl, face_tbl[faceid].index);
	if (face_tbl[faceid].local_f) {
		memcpy (api_frame, api_hdr, api_hdr_len);
		if ( payload && 0 < payload_len )
			memcpy (api_frame + api_hdr_len, payload, payload_len);

		ret = send (entry->sock, api_frame, (api_hdr_len + payload_len), 0);

	} else {
		ret = 0;
	}

	return (ret);
}
/*--------------------------------------------------------------------------------------
	Looks up the protocol type from the FD
----------------------------------------------------------------------------------------*/
int										/* Face-ID that is not used				*/
cef_face_get_protocol_from_fd (
	int fd
) {
	int i;

	for (i = 0 ; i < max_tbl_size ; i++) {
		if (face_tbl[i].fd != fd) {
			continue;
		}
		return (face_tbl[i].protocol);
	}

	return (CefC_Face_Type_Invalid);
}
/*--------------------------------------------------------------------------------------
	Closes all faces
----------------------------------------------------------------------------------------*/
void
cef_face_all_face_close (
	void
) {
	int i;

	for (i = 0 ; i < max_tbl_size ; i++) {
		if (face_tbl[i].fd) {
#ifdef CefC_Debug
			cef_dbg_write (CefC_Dbg_Finer, 
				"[face] Close the Face#%d (FD#%d)\n", i, face_tbl[i].fd);
#endif // CefC_Debug
			close (face_tbl[i].fd);
		}
	}

	free (face_tbl);
#ifdef CefC_Android
	/* Process for Android next running	*/
	face_tbl = NULL;
#endif // CefC_Android
	max_tbl_size = 0;
}
/*+++++ CEFORE-STATUS +++++*/
CefT_Hash_Handle*
cef_face_return_sock_table (
	void
) {
	return (&sock_tbl);
}
/*----- CEFORE-STATUS -----*/
/*--------------------------------------------------------------------------------------
	Looks up and creates the specified Face
----------------------------------------------------------------------------------------*/
static int									/* Face-ID									*/
cef_face_lookup_faceid (
	const char* destination, 				/* String of the destination address 		*/
	int protocol,							/* protoco (udp,tcp,local) 					*/
	int* create_f							/* set 1 if this face is new 				*/
) {
	struct addrinfo hints;
	struct addrinfo* res;
	struct addrinfo* cres;
	int err;
	char port_str[32];
	char peer[512];
	int sock;
	CefT_Sock* entry;
	int faceid;
	int index;
	int flag;
	int val;
	fd_set readfds;
	struct timeval timeout;
	int ret;
	
	if (create_f) {
		*create_f = 0;
	}

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	if (protocol != CefC_Face_Type_Tcp) {
		hints.ai_socktype = SOCK_DGRAM;
	} else {
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_NUMERICSERV;
	}

	sprintf (port_str, "%d", process_port_num);

	if ((err = getaddrinfo (destination, port_str, &hints, &res)) != 0) {
		cef_log_write (CefC_Log_Error, 
			"%s (getaddrinfo:%s)\n", __func__, gai_strerror(err));
		return (-1);
	}

	for (cres = res ; cres != NULL ; cres = res) {
		res = cres->ai_next;

		if (doing_ip_version != cres->ai_family) {
			cef_face_addrinfo_free (cres);
			continue;
		}
		sprintf (peer, "%s:%d", destination, protocol);
		entry = (CefT_Sock*) cef_hash_tbl_item_get (
					sock_tbl, (const unsigned char*) peer, strlen (peer));
		
		if (entry == NULL) {
			if (protocol != CefC_Face_Type_Tcp) {
				sock = socket (cres->ai_family, cres->ai_socktype, 0);
			} else {
				sock = socket (cres->ai_family, cres->ai_socktype, cres->ai_protocol);
			}
			
			if (sock < 0) {
				cef_face_addrinfo_free (cres);
				continue;
			}
			if (protocol != CefC_Face_Type_Udp) {
				flag = fcntl (sock, F_GETFL, 0);
				if (flag < 0) {
					close (sock);
					cef_face_addrinfo_free (cres);
					continue;
				}
				if (fcntl (sock, F_SETFL, flag | O_NONBLOCK) < 0) {
					close (sock);
					cef_face_addrinfo_free (cres);
					continue;
				}
				if (connect (sock, cres->ai_addr, cres->ai_addrlen) < 0) {
					/* NOP */;
				}
				val = 1;
				ioctl (sock, FIONBIO, &val);
				FD_ZERO (&readfds);
				FD_SET (sock, &readfds);
				timeout.tv_sec = 5;
				timeout.tv_usec = 0;
				ret = select (sock + 1, &readfds, NULL, NULL, &timeout);
				if (ret == 0) {
					close (sock);
					cef_face_addrinfo_free (cres);
					continue;
				} else {
					if (FD_ISSET (sock, &readfds)) {
						ret = recv (sock, port_str, 0, 0);
						if (ret == -1) {
							close (sock);
							cef_face_addrinfo_free (cres);
							continue;
						} else {
							/* NOP */;
						}
					}
				}
			}
			faceid = cef_face_unused_faceid_search ();
			if (faceid < 0) {
				close (sock);
				cef_face_addrinfo_free (cres);
				continue;
			}
			cres->ai_next = NULL;
			entry = cef_face_sock_entry_create (sock, cres->ai_addr, cres->ai_addrlen);
			entry->faceid = faceid;
			entry->protocol = (uint8_t) protocol;

			index = cef_hash_tbl_item_set (
				sock_tbl, (const unsigned char*) peer, strlen (peer), entry);

			if (index < 0) {
				close (sock);
				cef_face_sock_entry_destroy (entry);
				continue;
			}
			face_tbl[faceid].index = index;
			face_tbl[faceid].fd = entry->sock;
			face_tbl[faceid].protocol = (uint8_t) protocol;
			
			if (create_f) {
				*create_f = 1;
#ifdef CefC_Debug
				cef_dbg_write (CefC_Dbg_Finer, 
					"[face] Creation the new Face#%d (FD#%d) for %s\n", 
					entry->faceid, face_tbl[entry->faceid].fd, destination);
#endif // CefC_Debug
			}
		}
#ifndef CefC_Android
		freeaddrinfo (res);
#endif // CefC_Android
		return (entry->faceid);
	}

	return (-1);
}
/*--------------------------------------------------------------------------------------
	Looks up Face-ID that is not used
----------------------------------------------------------------------------------------*/
static int										/* Face-ID that is not used				*/
cef_face_unused_faceid_search (
	void
) {
	int i;

	for (i = assigned_faceid ; i < max_tbl_size ; i++) {
		if (face_tbl[i].fd != 0) {
			continue;
		}
		assigned_faceid = i + 1;
		return (i);
	}

	for (i = CefC_Face_Reserved ; i < assigned_faceid ; i++) {
		if (face_tbl[i].fd != 0) {
			continue;
		}
		assigned_faceid = i + 1;
		return (i);
	}
	assigned_faceid = CefC_Face_Reserved;
	return (-1);
}

/*--------------------------------------------------------------------------------------
	Deallocates the specified addrinfo
----------------------------------------------------------------------------------------*/
static void
cef_face_addrinfo_free (
	struct addrinfo* ai							/* addrinfo to free 					*/
) {
	if (ai) {
		free (ai);
	}
}
/*--------------------------------------------------------------------------------------
	Creates a new entry of Socket Table
----------------------------------------------------------------------------------------*/
static CefT_Sock*								/* the created new entry				*/
cef_face_sock_entry_create (
	int sock, 									/* file descriptor to register			*/
	struct sockaddr* ai_addr,
	socklen_t ai_addrlen
) {
	CefT_Sock* entry;

	entry = (CefT_Sock*) malloc (sizeof (CefT_Sock));
	entry->ai_addr = ai_addr;
	entry->ai_addrlen = ai_addrlen;
	entry->sock = sock;
	entry->faceid = -1;

	return (entry);
}
/*--------------------------------------------------------------------------------------
	Destroy the specified entry of Socket Table
----------------------------------------------------------------------------------------*/
static void
cef_face_sock_entry_destroy (
	CefT_Sock* entry							/* the entry to destroy					*/
) {

	if (entry->ai_addr) {
		free (entry->ai_addr);
	}
	free (entry);
}