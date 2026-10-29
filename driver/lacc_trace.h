/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM lacc

#if !defined(_LACC_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _LACC_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(lacc_submit,
	    TP_PROTO(u64 seq, u8 opcode, u32 ndesc),
	    TP_ARGS(seq, opcode, ndesc),
	    TP_STRUCT__entry(__field(u64, seq)
			     __field(u8, opcode)
			     __field(u32, ndesc)),
	    TP_fast_assign(__entry->seq = seq;
			   __entry->opcode = opcode;
			   __entry->ndesc = ndesc;),
	    TP_printk("seq=%llu opcode=%u ndesc=%u", __entry->seq, __entry->opcode, __entry->ndesc)
);

TRACE_EVENT(lacc_complete,
	    TP_PROTO(u64 seq, u16 status, u8 result, u64 device_ns),
	    TP_ARGS(seq, status, result, device_ns),
	    TP_STRUCT__entry(__field(u64, seq)
			     __field(u16, status)
			     __field(u8, result)
			     __field(u64, device_ns)),
	    TP_fast_assign(__entry->seq = seq;
			   __entry->status = status;
			   __entry->result = result;
			   __entry->device_ns = device_ns;),
	    TP_printk("seq=%llu status=%u result=%u device_ns=%llu", __entry->seq, __entry->status,
		      __entry->result, __entry->device_ns)
);

TRACE_EVENT(lacc_cq,
	    TP_PROTO(unsigned int n, u32 cq_head),
	    TP_ARGS(n, cq_head),
	    TP_STRUCT__entry(__field(unsigned int, n)
			     __field(u32, cq_head)),
	    TP_fast_assign(__entry->n = n;
			   __entry->cq_head = cq_head;),
	    TP_printk("cqes=%u head=%u", __entry->n, __entry->cq_head)
);

TRACE_EVENT(lacc_reset,
	    TP_PROTO(const char *why),
	    TP_ARGS(why),
	    TP_STRUCT__entry(__string(why, why)),
	    TP_fast_assign(__assign_str(why);),
	    TP_printk("%s", __get_str(why))
);

#endif /* _LACC_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE lacc_trace
#include <trace/define_trace.h>
