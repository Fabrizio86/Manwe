//
// Created by Fabrizio Paino on 2022-03-29.
//

#ifndef YARN_STOPEXECUTIONEXCEPTION_HPP
#define YARN_STOPEXECUTIONEXCEPTION_HPP

#include <exception>

/**
 * @class StopExecutionException
 * @brief Cooperative stop signal a task may throw to unwind itself cleanly.
 *
 * The executor used to deliver this asynchronously via a POSIX signal, which
 * was undefined behaviour; that path has been removed. Tasks that wish to
 * abort cooperatively can throw this from @c ITask::run and the worker will
 * forward it through @c ITask::exception.
 */
class StopExecutionException : public std::exception {
public:
    const char *what() const noexcept override;
};

#endif //YARN_STOPEXECUTIONEXCEPTION_HPP
