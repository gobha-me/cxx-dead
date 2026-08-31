#pragma once

void external_api();
void external_initializer_target();

inline int external_initialized_value = (external_initializer_target(), 1);
