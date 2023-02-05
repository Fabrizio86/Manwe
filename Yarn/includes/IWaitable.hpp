//
// Created by Fabrizio Paino on 2022-01-16.
//

#ifndef YARN_IWAITABLE_HPP
#define YARN_IWAITABLE_HPP

namespace YarnBall {

    class IWaitable : public ITask {
    public:
        virtual ~IWaitable()  = default;

        virtual void wait() = 0;

        virtual bool hasFailed() = 0;

        virtual std::string errorMessage() = 0;
    };

    using sIWaitable = std::shared_ptr<IWaitable>;

}

#endif //YARN_IWAITABLE_HPP
