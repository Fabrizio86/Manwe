//
// Created by Fabrizio Paino on 2022-03-29.
//

#include "StopExecutionException.h"

const char *StopExecutionException::what() const _NOEXCEPT {
    return "Operation timed out";
}