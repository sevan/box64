/*******************************************************************
 * File automatically generated by rebuild_wrappers.py (v2.2.0.16) *
 *******************************************************************/
#ifndef __wrappeddbusglib1TYPES_H_
#define __wrappeddbusglib1TYPES_H_

#ifndef LIBNAME
#error You should only #include this file inside a wrapped*.c file
#endif
#ifndef ADDED_FUNCTIONS
#define ADDED_FUNCTIONS() 
#endif

typedef void (*vFppp_t)(void*, void*, void*);
typedef void (*vFpppp_t)(void*, void*, void*, void*);
typedef void (*vFppppp_t)(void*, void*, void*, void*, void*);
typedef void* (*pFpppppiV_t)(void*, void*, void*, void*, void*, int32_t, ...);
typedef void* (*pFpppppiiV_t)(void*, void*, void*, void*, void*, int32_t, int32_t, ...);

#define SUPER() ADDED_FUNCTIONS() \
	GO(dbus_g_type_collection_value_iterate, vFppp_t) \
	GO(dbus_g_type_map_value_iterate, vFppp_t) \
	GO(dbus_g_proxy_disconnect_signal, vFpppp_t) \
	GO(dbus_g_proxy_connect_signal, vFppppp_t) \
	GO(dbus_g_proxy_begin_call, pFpppppiV_t) \
	GO(dbus_g_proxy_begin_call_with_timeout, pFpppppiiV_t)

#endif // __wrappeddbusglib1TYPES_H_
