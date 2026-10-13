idf_component_register(SRCS "app_game_runtime.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES examples_common
    PRIV_REQUIRES app_game raylib-lite-engine)
target_compile_definitions(${COMPONENT_LIB} PRIVATE RAYLIB_LITE_GAME_NATIVE=1)
