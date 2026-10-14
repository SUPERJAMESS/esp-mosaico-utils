file(SIZE "${RECOVERY_BINARY}" recovery_bytes)
math(EXPR recovery_limit "${RECOVERY_SLOT_SIZE}")
if(recovery_bytes GREATER recovery_limit)
    message(FATAL_ERROR
        "Recovery image is ${recovery_bytes} bytes, exceeding its ${recovery_limit}-byte slot")
endif()
math(EXPR recovery_free "${recovery_limit} - ${recovery_bytes}")
message(STATUS "Recovery: ${recovery_bytes} bytes, ${recovery_free} bytes free in fixed slot")
