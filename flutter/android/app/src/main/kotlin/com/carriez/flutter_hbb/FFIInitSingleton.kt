package com.carriez.flutter_hbb

import ffi.FFI

import android.content.Context
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Static singleton class to initialize NDK contexts in FFI for hardware codecs
 */

enum class FFIInitSingleton {
    INSTANCE;

    private val initCtxCalled = AtomicBoolean(false)
    private val initAvCalled = AtomicBoolean(false)

    fun initCtx(ctx: Context): Int {
        if (initCtxCalled.compareAndSet(false, true)) {
            return FFI.initCtx(ctx)
        }
        return -1
    }

    fun initAv(): Int {
        if (initAvCalled.compareAndSet(false, true)) {
            return FFI.initAv()
        }
        return -1
    }
}
