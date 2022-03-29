//
// Created by Fabrizio Paino on 2022-03-29.
//

#ifndef YARN_STOPEXECUTIONEXCEPTION_H
#define YARN_STOPEXECUTIONEXCEPTION_H

#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

class StopExecutionException : std::exception {
    const char *what()  const _NOEXCEPT override;
};

#endif //YARN_STOPEXECUTIONEXCEPTION_H
