if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  add_custom_target(qemu_smoke
    COMMAND ${PROJECT_DIR}/scripts/qemu_smoke_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(qemu_stress
    COMMAND ${PROJECT_DIR}/scripts/qemu_stress_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(qemu_soak
    COMMAND ${PROJECT_DIR}/scripts/qemu_soak_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(qemu_regress
    COMMAND ${PROJECT_DIR}/scripts/qemu_regressions_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(qemu_applets
    COMMAND ${PROJECT_DIR}/scripts/qemu_applets_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(hil_applets
    COMMAND ${PROJECT_DIR}/scripts/hil_applets_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(hil_smoke
    COMMAND ${PROJECT_DIR}/scripts/hil_smoke_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(hil_gate
    COMMAND ${PROJECT_DIR}/scripts/hil_gate_esp.py --port $ENV{HIL_PORT}
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(static_analysis
    COMMAND ${PROJECT_DIR}/scripts/static_analysis.sh
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )

  add_custom_target(abi_check
    COMMAND ${PROJECT_DIR}/scripts/check_hostabi_abi.py --require-generated-checks
    DEPENDS xv6fs_img_gen ${PROJECT_DIR}/kernel/runtime/shell_runtime_shell.inc
    USES_TERMINAL
  )

  add_custom_target(ci_gate
    COMMAND ${PROJECT_DIR}/scripts/ci_gate_esp.py
    DEPENDS xv6fs_img_gen
    USES_TERMINAL
  )
endif()
