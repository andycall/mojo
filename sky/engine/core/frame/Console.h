/*
 * Copyright (C) 2007 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef SKY_ENGINE_CORE_FRAME_CONSOLE_H_
#define SKY_ENGINE_CORE_FRAME_CONSOLE_H_

#include "sky/engine/core/frame/ConsoleBase.h"
#include "sky/engine/core/frame/DOMWindowProperty.h"
#include "sky/engine/platform/heap/Handle.h"
#include "sky/engine/wtf/Forward.h"
#include "sky/engine/wtf/PassRefPtr.h"
#include "sky/engine/wtf/RefCounted.h"

namespace blink {

class LocalFrame;
class MemoryInfo;
class Page;
class ScriptArguments;

class Console final : public ConsoleBase, public DOMWindowProperty {
    DEFINE_WRAPPERTYPEINFO();
public:
    static PassRefPtr<Console> create(LocalFrame* frame)
    {
        return adoptRef(new Console(frame));
    }
    virtual ~Console();

    PassRefPtr<MemoryInfo> memory() const;

protected:
    virtual ExecutionContext* context() override;
    virtual void reportMessageToConsole(PassRefPtr<ConsoleMessage>) override;

private:
    explicit Console(LocalFrame*);
};

} // namespace blink

#endif  // SKY_ENGINE_CORE_FRAME_CONSOLE_H_
