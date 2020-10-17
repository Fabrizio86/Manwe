//
// Created by Administrator on 2020-10-17.
//

#ifndef YARNS_ASYNCTASK_H
#define YARNS_ASYNCTASK_H

#include "Definitions.h"

namespace YarnBall {

class AsyncTask : public ITask {
public:
    explicit AsyncTask(Task task);

    void run() override;

    void exception(std::exception_ptr exception) override;

private:
    Task task;
};

}

#endif //YARNS_ASYNCTASK_H
