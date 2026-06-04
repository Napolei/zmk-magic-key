if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(ZMK_MAGIC_KEY app PRIVATE behaviors/behavior_magic_key.c)
endif()
