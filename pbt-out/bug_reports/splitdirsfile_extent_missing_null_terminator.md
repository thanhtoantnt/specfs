# splitDirsFile (extent) missing NULL terminator
Same bug as delay_alloc/pre_alloc/inline_data: splitDirsFile never writes dirname[num] = NULL.
Confirmed by terminates_uninitialized_output property.
