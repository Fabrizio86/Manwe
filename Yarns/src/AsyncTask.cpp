//
// Created by Administrator on 2020-10-17.
//

#include "AsyncTask.h"

#include <utility>

namespace YarnBall {

    void AsyncTask::run() {
        try {
            task();
        }
        catch (...) {}
    }

    void AsyncTask::exception(std::exception_ptr exception) {}

    AsyncTask::AsyncTask(Task task) : task(std::move(task)) {}

}