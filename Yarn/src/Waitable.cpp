//
// Created by Fabrizio Paino on 2022-01-16.
//

#include "Waitable.h"
#include "YarnBall.h"

namespace YarnBall {
    Waitable::Waitable(YarnBall::Operation operation) : operation(std::move(operation)) {}

    void Waitable::wait() {
        std::this_thread::yield();
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&done = this->done] { return done; });
    }

    bool Waitable::hasFailed() {
        return this->failed;
    }

    void Waitable::run() {
        this->operation();
        this->notifyComplition();
    }

    void Waitable::notifyComplition() {
        this->done = true;
        this->cv.notify_all();
    }

    void Waitable::exception(std::exception_ptr exception) {
        this->failed = true;

        try { std::rethrow_exception(exception); }
        catch (const std::exception &e) { this->error = e.what(); }
        catch (const std::string &e) { this->error = e; }
        catch (const char *e) { this->error = e; }
        catch (...) { this->error = "Unknown error"; }

        this->notifyComplition();
    }

    std::string Waitable::errorMessage() {
        return this->error;
    }
}