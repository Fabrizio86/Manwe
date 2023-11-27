//
// Created by Fabrizio Paino on 2023-03-13.
// Copyright (c) 2023 Aedifex Solutions Inc. All rights reserved.
//

#ifndef MANWE_IEVENT_HPP
#define MANWE_IEVENT_HPP

#include <string>

namespace Telegraph {

    class iEventObject {
    public:
        virtual ~iEventObject() = default;

        virtual std::string name() = 0;
    };

    class iEvent {
    public:
        virtual ~iEvent() = default;

        virtual std::string name() = 0;

        virtual void trigger(iEventObject* data) = 0;
    };

}
#endif //MANWE_IEVENT_HPP
