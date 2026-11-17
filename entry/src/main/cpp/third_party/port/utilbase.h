/*
 * utilbase.h — OHOS port of saki's UVCCamera utilbase.h
 * Maps the Android logging macros used by libusb/libuvc (saki fork)
 * onto OpenHarmony hilog. No jni.h / android/log.h dependencies.
 */

#ifndef UTILBASE_H_
#define UTILBASE_H_

#include <unistd.h>
#include <libgen.h>
#include <assert.h>
#include <hilog/log.h>

#ifndef LOG_TAG
#define LOG_TAG "uvccam"
#endif

// 对齐 Android 侧 localdefines.h：启用 libuvc 的 JPEG(MJPEG) 支持
#define LIBUVC_HAS_JPEG

#define		SAFE_FREE(p)				{ if (p) { free((p)); (p) = NULL; } }
#define		SAFE_DELETE(p)				{ if (p) { delete (p); (p) = NULL; } }
#define		SAFE_DELETE_ARRAY(p)		{ if (p) { delete [](p); (p) = NULL; } }
#define		NUM_ARRAY_ELEMENTS(p)		((int) sizeof(p) / sizeof(p[0]))

#if defined(__GNUC__)
// the macro for branch prediction optimaization for gcc(-O2/-O3 required)
#define		CONDITION(cond)				((__builtin_expect((cond)!=0, 0)))
#define		LIKELY(x)					((__builtin_expect(!!(x), 1)))	// x is likely true
#define		UNLIKELY(x)					((__builtin_expect(!!(x), 0)))	// x is likely false
#else
#define		CONDITION(cond)				((cond))
#define		LIKELY(x)					((x))
#define		UNLIKELY(x)					((x))
#endif

#include <assert.h>
#define CHECK(CONDITION) { bool RES = (CONDITION); assert(RES); }
#define CHECK_EQ(X, Y) { bool RES = (X == Y); assert(RES); }
#define CHECK_NE(X, Y) { bool RES = (X != Y); assert(RES); }
#define CHECK_GE(X, Y) { bool RES = (X >= Y); assert(RES); }
#define CHECK_GT(X, Y) { bool RES = (X > Y); assert(RES); }
#define CHECK_LE(X, Y) { bool RES = (X <= Y); assert(RES); }
#define CHECK_LT(X, Y) { bool RES = (X < Y); assert(RES); }

// LOGV/LOGD are silenced by default: libusb/libuvc call them in hot paths
// (ENTER()/RETURN()/EXIT() expand to LOGD on every function call).
// LOGI/LOGW/LOGE/LOGF/MARK are always active via hilog.
#define LOGV(...)
#define LOGV_IF(cond, ...)
#define LOGD(...)
#define LOGD_IF(cond, ...)

#define LOGI(FMT, ...) OH_LOG_Print(LOG_APP, LOG_INFO,  0, LOG_TAG, "[%s:%d:%s]:" FMT, basename(__FILE__), __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define LOGW(FMT, ...) OH_LOG_Print(LOG_APP, LOG_WARN,  0, LOG_TAG, "[%s:%d:%s]:" FMT, basename(__FILE__), __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define LOGE(FMT, ...) OH_LOG_Print(LOG_APP, LOG_ERROR, 0, LOG_TAG, "[%s:%d:%s]:" FMT, basename(__FILE__), __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define LOGF(FMT, ...) OH_LOG_Print(LOG_APP, LOG_FATAL, 0, LOG_TAG, "[%s:%d:%s]:" FMT, basename(__FILE__), __LINE__, __FUNCTION__, ## __VA_ARGS__)

#define LOGI_IF(cond, ...) \
	( (CONDITION(cond)) \
		? LOGI(__VA_ARGS__) \
		: (0) )
#define LOGW_IF(cond, ...) \
	( (CONDITION(cond)) \
		? LOGW(__VA_ARGS__) \
		: (0) )
#define LOGE_IF(cond, ...) \
	( (CONDITION(cond)) \
		? LOGE(__VA_ARGS__) \
		: (0) )
#define LOGF_IF(cond, ...) \
	( (CONDITION(cond)) \
		? LOGF(__VA_ARGS__) \
		: (0) )

#define		LOG_ALWAYS_FATAL_IF(cond, ...) \
				( (CONDITION(cond)) \
				? ((void)LOGF(__VA_ARGS__), abort()) \
				: (void)0 )
#define		LOG_ALWAYS_FATAL(...) \
				( ((void)LOGF(__VA_ARGS__), abort()) )
#ifndef		LOG_ASSERT
#define		LOG_ASSERT(cond, ...) LOG_ALWAYS_FATAL_IF(!(cond), ## __VA_ARGS__)
#endif
#ifndef		LOG_FATAL_IF
#define		LOG_FATAL_IF(cond, ...) LOG_ALWAYS_FATAL_IF(cond, ## __VA_ARGS__)
#endif
#ifndef		LOG_FATAL
#define		LOG_FATAL(...) LOG_ALWAYS_FATAL(__VA_ARGS__)
#endif

#define		ENTER()				LOGD("begin")
#define		RETURN(code,type)	{type RESULT = code; LOGD("end (%d)", (int)RESULT); return RESULT;}
#define		RET(code)			{LOGD("end"); return code;}
#define		EXIT()				{LOGD("end"); return;}
#define		PRE_EXIT()			LOGD("end")

#define MARK(FMT, ...) OH_LOG_Print(LOG_APP, LOG_INFO, 0, LOG_TAG, "[%s:%d:%s]:" FMT, basename(__FILE__), __LINE__, __FUNCTION__, ## __VA_ARGS__)

#define LITERAL_TO_STRING_INTERNAL(x)    #x
#define LITERAL_TO_STRING(x) LITERAL_TO_STRING_INTERNAL(x)

#define TRESPASS() \
		LOG_ALWAYS_FATAL(                                       \
			__FILE__ ":" LITERAL_TO_STRING(__LINE__)            \
			" Should not be here.");

#endif /* UTILBASE_H_ */
