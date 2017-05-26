/*
 * AudioInputProcessorTest.cpp
 *
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/// @file AudioInputProcessorTest.cpp
#include <gtest/gtest.h>
#include <string>
#include <future>
#include <fstream>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "ACL/AVSConnectionManager.h"
#include "ACL/Transport/HTTP2MessageRouter.h"
#include "ACL/Values.h"
#include "ADSL/DirectiveSequencer.h"
#include "ADSL/MessageInterpreter.h"
#include "AFML/Channel.h"
#include "AFML/FocusManager.h"
#include "AuthDelegate/AuthDelegate.h"
#include "AIP/AudioInputProcessor.h"
#include "AIP/AudioProvider.h"
#include "AIP/Initiator.h"
#include "AVSCommon/AVS/Attachment/InProcessAttachmentWriter.h"
#include "AVSCommon/AVS/BlockingPolicy.h"
#include "AVSCommon/AVS/MessageRequest.h"
#include "AVSCommon/ExceptionEncounteredSenderInterface.h"
#include "AVSCommon/JSON/JSONUtils.h"
#include "AVSCommon/SDKInterfaces/ChannelObserverInterface.h"
#include "AVSCommon/SDKInterfaces/ContextManagerInterface.h"
#include "AVSCommon/SDKInterfaces/DirectiveHandlerInterface.h"
#include "AVSCommon/SDKInterfaces/DirectiveHandlerResultInterface.h"
#include "AVSUtils/Initialization/AlexaClientSDKInit.h"
#include "AVSUtils/Logger/LogEntry.h"
#include "ContextManager/ContextManager.h"
#include "Integration/AuthObserver.h"
#include "Integration/ClientMessageHandler.h"
#include "Integration/ConnectionStatusObserver.h"
#include "Integration/ObservableMessageRequest.h"
#include "Integration/AipStateObserver.h"
#include "Integration/TestMessageSender.h"

#ifdef KWD_KITTAI
#include "KittAi/KittAiKeyWordDetector.h"
#endif

namespace alexaClientSDK {
namespace integration {

using namespace alexaClientSDK::acl;
using namespace alexaClientSDK::adsl;
using namespace alexaClientSDK::authDelegate;
using namespace alexaClientSDK::avsCommon;
using namespace alexaClientSDK::avsCommon::avs;
using namespace alexaClientSDK::avsCommon::avs::attachment;
using namespace alexaClientSDK::avsCommon::sdkInterfaces;
using namespace alexaClientSDK::avsUtils::initialization;
using namespace capabilityAgent::aip;
using namespace sdkInterfaces;
using namespace avsCommon::utils::sds;
using namespace afml;
using namespace contextManager;

// This is a 16 bit 16 kHz little endian linear PCM audio file of "Tell me a Joke" to be recognized.
static const std::string JOKE_AUDIO_FILE = "/recognize_joke_test.wav";
// This is a 16 bit 16 kHz little endian linear PCM audio file of "Wikipedia" to be recognized.
static const std::string WIKI_AUDIO_FILE = "/recognize_wiki_test.wav";
// This is a 16 bit 16 kHz little endian linear PCM audio file of "Lions" to be recognized.
static const std::string LIONS_AUDIO_FILE = "/recognize_lions_test.wav";
// This is a 16 bit 16 kHz little endian linear PCM audio file of silence to be recognized.
static const std::string SILENCE_AUDIO_FILE = "/recognize_silence_test.wav";
// This is a 16 bit 16 kHz little endian linear PCM audio file of "Alexa, Tell me a Joke" to be recognized.
static const std::string ALEXA_JOKE_AUDIO_FILE = "/alexa_recognize_joke_test.wav";
// This is a 16 bit 16 kHz little endian linear PCM audio file of "Alexa, Wikipedia" to be recognized.
static const std::string ALEXA_WIKI_AUDIO_FILE = "/alexa_recognize_wiki_test.wav";
// This is a 16 bit 16 kHz little endian linear PCM audio file of "Alexa" then silence to be recognized.
static const std::string ALEXA_SILENCE_AUDIO_FILE = "/alexa_recognize_silence_test.wav";

// This string to be used for Speak Directives which use the NAMESPACE_SPEECH_SYNTHESIZER namespace.
static const std::string NAME_VOLUME_STATE = "VolumeState";
// This string to be used for Speak Directives which use the NAMESPACE_SPEECH_SYNTHESIZER namespace.
static const std::string NAME_SPEAK = "Speak";
// This string to be used for ExpectSpeech Directives which use the NAMESPACE_SPEECH_RECOGNIZER namespace.
static const std::string NAME_EXPECT_SPEECH = "ExpectSpeech";
// This string to be used for ExpectSpeechTimedOut Events which use the NAMESPACE_SPEECH_RECOGNIZER namespace.
static const std::string NAME_EXPECT_SPEECH_TIMED_OUT = "ExpectSpeechTimedOut";
// This string to be used for SetMute Directives which use the NAMESPACE_SPEAKER namespace.
static const std::string NAME_SET_MUTE = "SetMute";
// This string to be used for Play Directives which use the NAMESPACE_AUDIO_PLAYER namespace.
static const std::string NAME_PLAY = "Play";
// This string to be used for StopCapture Directives which use the NAMESPACE_SPEECH_RECOGNIZER namespace.
static const std::string NAME_STOP_CAPTURE = "StopCapture";
// This string to be used for Recognize Directives which use the NAMESPACE_SPEECH_RECOGNIZER namespace.
static const std::string NAME_RECOGNIZE = "Recognize";
// This String to be used to register the SpeechRecognizer namespace to a DirectiveHandler.
static const std::string NAMESPACE_SPEECH_RECOGNIZER = "SpeechRecognizer";
// This String to be used to register the SpeechSynthesizer namespace to a DirectiveHandler.
static const std::string NAMESPACE_SPEECH_SYNTHESIZER = "SpeechSynthesizer";
// This String to be used to register the AudioPlayer namespace to a DirectiveHandler. 
static const std::string NAMESPACE_AUDIO_PLAYER = "AudioPlayer";
// This String to be used to register the Speaker namespace to a DirectiveHandler. 
static const std::string NAMESPACE_SPEAKER = "Speaker";
// This pair connects a Speak name and SpeechSynthesizer namespace for use in DirectiveHandler registration.
static const NamespaceAndName SPEAK_PAIR = {NAMESPACE_SPEECH_SYNTHESIZER, NAME_SPEAK};
// This pair connects a ExpectSpeech name and SpeechRecognizer namespace for use in DirectiveHandler registration.
static const NamespaceAndName EXPECT_SPEECH_PAIR = {NAMESPACE_SPEECH_RECOGNIZER, NAME_EXPECT_SPEECH};
// This pair connects a SetMute name and Speaker namespace for use in DirectiveHandler registration.
static const NamespaceAndName SET_MUTE_PAIR = {NAMESPACE_SPEAKER, NAME_SET_MUTE};
// This pair connects a SetMute name and Speaker namespace for use in DirectiveHandler registration.
static const NamespaceAndName VOLUME_STATE_PAIR = {NAMESPACE_SPEAKER, NAME_VOLUME_STATE};
// This pair connects a Play name and AudioPlayer namespace for use in DirectiveHandler registration.
static const NamespaceAndName PLAY_PAIR = {NAMESPACE_AUDIO_PLAYER, NAME_PLAY};
// This pair connects a StopCapture name and SpeechRecognizer namespace for use in DirectiveHandler registration.
static const NamespaceAndName STOP_CAPTURE_PAIR = {NAMESPACE_SPEECH_RECOGNIZER, NAME_STOP_CAPTURE};
/// The dialog Channel name used in intializing the FocusManager.
static const std::string DIALOG_CHANNEL_NAME = "Dialog";
/// The alerts Channel name used in intializing the FocusManager.
static const std::string ALERTS_CHANNEL_NAME = "Alerts";
/// The content Channel name used in intializing the FocusManager.
static const std::string CONTENT_CHANNEL_NAME = "Content";
/// The priority of the dialog Channel used in intializing the FocusManager.
static const unsigned int DIALOG_CHANNEL_PRIORITY = 10;
/// The priority of the alerts Channel used in intializing the FocusManager.
static const unsigned int ALERTS_CHANNEL_PRIORITY = 20;
/// The priority of the content Channel used in intializing the FocusManager.
static const unsigned int CONTENT_CHANNEL_PRIORITY = 30;
/// Sample dialog activity id.
static const std::string DIALOG_ACTIVITY_ID = "Dialog";
/// Sample alerts activity id.
static const std::string ALARM_ACTIVITY_ID = "Alarms";
/// Sample content activity id.
static const std::string CONTENT_ACTIVITY_ID = "Content";

// This Integer to be used to specify a timeout in seconds for a AIP to change states.
static const std::chrono::seconds AUDIO_FILE_TIMEOUT_DURATION(7);
// This Integer to be used to specify a timeout in seconds for a directive to reach the DirectiveHandler.
static const std::chrono::seconds DIRECTIVE_TIMEOUT_DURATION(7);
// This Integer to be used when it is expected the duration will timeout.
static const std::chrono::seconds WANTING_TIMEOUT_DURATION(2);

/// The name of the resource file required for Kitt.ai.
static const std::string RESOURCE_FILE = "/KittAiModels/common.res";
/// The name of the Alexa model file for Kitt.ai.
static const std::string MODEL_FILE = "/KittAiModels/alexa.umdl"; 
/// The keyword associated with alexa.umdl.
static const std::string MODEL_KEYWORD = "ALEXA";

/// JSON key to get the directive object of a message.
static const std::string JSON_MESSAGE_DIRECTIVE_KEY = "directive";
/// JSON key to get the header object of a message.
static const std::string JSON_MESSAGE_HEADER_KEY = "header";
/// JSON key to get the namespace value of a header.
static const std::string JSON_MESSAGE_NAMESPACE_KEY = "namespace";
/// JSON key to get the name value of a header.
static const std::string JSON_MESSAGE_NAME_KEY = "name";
/// JSON key to get the messageId value of a header.
static const std::string JSON_MESSAGE_MESSAGE_ID_KEY = "messageId";
/// JSON key to get the dialogRequestId value of a header.
static const std::string JSON_MESSAGE_DIALOG_REQUEST_ID_KEY = "dialogRequestId";
/// JSON key to get the payload object of a message.
static const std::string JSON_MESSAGE_PAYLOAD_KEY = "payload";

/**
 * The sensitivity to the keyword in the model. Set to 0.6 as this is what was described as optimal on the Kitt.ai 
 * Github page.
 */
#ifdef KWD_KITTAI
static const double KITTAI_SENSITIVITY = 0.6;
#endif
/// The compatible encoding for Kitt.ai.
static const avsCommon::AudioFormat::Encoding COMPATIBLE_ENCODING = avsCommon::AudioFormat::Encoding::LPCM;
/// The compatible endianness for Kitt.ai.
static const avsCommon::AudioFormat::Endianness COMPATIBLE_ENDIANNESS = avsCommon::AudioFormat::Endianness::LITTLE;
/// The compatible sample rate for Kitt.ai.
static const unsigned int COMPATIBLE_SAMPLE_RATE = 16000;
/// The compatible bits per sample for Kitt.ai.
static const unsigned int COMPATIBLE_SAMPLE_SIZE_IN_BITS = 16;
/// The compatible number of channels for Kitt.ai 
static const unsigned int COMPATIBLE_NUM_CHANNELS = 1;

/// String to identify log entries originating from this file.
static const std::string TAG("AlexaDirectiveSequencerLibraryTest");

/// Path to configuration file (from command line arguments).
std::string configPath;
/// Path to directory containing input data (from command line arguments).
std::string inputPath;

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) ::alexaClientSDK::avsUtils::logger::LogEntry(TAG, event)

class tapToTalkButton{
    public:
    bool startRecognizing(std::shared_ptr<AudioInputProcessor> aip, std::shared_ptr<AudioProvider> audioProvider) {
        return aip->recognize(*audioProvider, Initiator::TAP).get();
    }
};

class holdToTalkButton{
public:
    bool startRecognizing(std::shared_ptr<AudioInputProcessor> aip,
        std::shared_ptr<AudioProvider> audioProvider) {
        return aip->recognize(*audioProvider, Initiator::PRESS_AND_HOLD).get();
    }
    
    bool stopRecognizing(std::shared_ptr<AudioInputProcessor> aip){
        return aip->stopCapture().get();
    }
};

#ifdef KWD
class wakeWordTrigger  : public KeyWordObserverInterface{
    public:
        wakeWordTrigger(AudioFormat compatibleAudioFormat, std::shared_ptr<AudioInputProcessor> aip) {
            m_compatibleAudioFormat = compatibleAudioFormat;
            m_aip = aip; 
        }
    void onKeyWordDetected(
        std::shared_ptr<AudioInputStream> stream,
        std::string keyword,
        AudioInputStream::Index beginIndex,
        AudioInputStream::Index endIndex) {

            keyWordDetected = true; 
            ASSERT_NE(nullptr, stream);
            bool alwaysReadable = true;
            bool canOverride = true;
            bool canBeOverridden = true;
            auto audioProvider = AudioProvider( stream, m_compatibleAudioFormat,
            ASRProfile::NEAR_FIELD, alwaysReadable, !canOverride, canBeOverridden);

            if(m_aip) {
                AudioInputStream::Index aipBegin = AudioInputProcessor::INVALID_INDEX;
                AudioInputStream::Index aipEnd = AudioInputProcessor::INVALID_INDEX;

                if (endIndex != KeyWordObserverInterface::UNSPECIFIED_INDEX) {
                    if (beginIndex != KeyWordObserverInterface::UNSPECIFIED_INDEX) {
                        // If we know where the keyword starts and ends, pass both of those along to AIP.
                        aipBegin = beginIndex;
                        aipEnd = endIndex;
                    } else {
                        // If we only know where the keyword ends, AIP should begin recording there.
                        aipBegin = endIndex;
                    }
                }
                // Else we don't have any indices to pass along; AIP will begin recording ASAP.
                m_aip->recognize(audioProvider, Initiator::TAP, aipBegin, aipEnd, keyword);
            }
        }

    bool keyWordDetected = false; 
    AudioFormat m_compatibleAudioFormat;
    std::shared_ptr<AudioInputProcessor> m_aip; 
};
#endif

class testStateProvider : public StateProviderInterface{
public: 
    testStateProvider(std::shared_ptr<avsCommon::sdkInterfaces::ContextManagerInterface> contextManager) {
        m_contextManager = contextManager;
    }
        ~testStateProvider() {
    }
    void provideState(const unsigned int stateRequestToken) {
        std::ostringstream context;
        context << R"({)"
                   R"("volume":)" << 50 << R"(,)"
            << R"("muted":)" << false << R"(})";

            m_contextManager->setState(VOLUME_STATE_PAIR,
            context.str(), avsCommon::sdkInterfaces::StateRefreshPolicy::ALWAYS,
            stateRequestToken);
        m_stateRequested = true;
    }
    bool checkStateRequested() {
        bool savedResult = false; 
        if(m_stateRequested) {
            savedResult = true;
            m_stateRequested = false; 
        }
        return savedResult; 
    }
private:
   bool m_stateRequested  = false;
   std::shared_ptr<avsCommon::sdkInterfaces::ContextManagerInterface> m_contextManager;

};

/// A test observer that mocks out the ChannelObserverInterface##onFocusChanged() call.
class TestClient : public ChannelObserverInterface {
public:
    /**
     * Constructor.
     */
    TestClient() :
        m_focusState(FocusState::NONE) {
    }

    /**
     * Implementation of the ChannelObserverInterface##onFocusChanged() callback.
     *
     * @param focusState The new focus state of the Channel observer.
     */
    void onFocusChanged(FocusState focusState) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.push_back(focusState);
        m_focusState = focusState;
        m_wakeTrigger.notify_all();
    }
 
    /**
     * Waits for the ChannelObserverInterface##onFocusChanged() callback.
     *
     * @param timeout The amount of time to wait for the callback.
     * @param focusChanged An output parameter that notifies the caller whether a callback occurred.
     * @return Returns @c true if the callback occured within the timeout period and @c false otherwise.
     */
    FocusState waitForFocusChange(std::chrono::milliseconds timeout) {
        FocusState ret;
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_wakeTrigger.wait_for(lock, timeout, [this]() { return !m_queue.empty(); })) {
            ret = FocusState::NONE;
            return ret;
        }
        ret = m_queue.front();
        m_queue.pop_front();
        return ret;
    }

private:
    /// The focus state of the observer.
    FocusState m_focusState;

    /// A lock to guard against focus state changes.
    std::mutex m_mutex;

    /// Trigger to wake up waitForNext calls.
    std::condition_variable m_wakeTrigger;
    /// Queue of received directives that have not been waited on.
    std::deque<FocusState> m_queue;
};

/**
 * TestDirectiveHandler is a mock of the @c DirectiveHandlerInterface and
 * @c ExceptionEncounteredSenderInterface allows tests to wait for invocations upon those interfaces and
 * inspect the parameters of those invocations.
 */
class TestDirectiveHandler : public DirectiveHandlerInterface, public ExceptionEncounteredSenderInterface {
public:
    void handleDirectiveImmediately(std::shared_ptr<avsCommon::AVSDirective> directive) override;

    void preHandleDirective(
            std::shared_ptr<avsCommon::AVSDirective> directive,
            std::unique_ptr<DirectiveHandlerResultInterface> result) override;

    bool handleDirective(const std::string& messageId) override;

    void cancelDirective(const std::string& messageId) override;

    void onDeregistered() override;
    
    void sendExceptionEncountered(
        const std::string& unparsedDirective,
        ExceptionErrorType error,
        const std::string& message) override;

    /**
     * Class defining the parameters to calls to the mocked interfaces.
     */
    class DirectiveParams {
    public:
        /**
         * Constructor.
         */
        DirectiveParams();

        /**
         * Return whether this DirectiveParams is of type 'UNSET'.
         *
         * @return Whether this DirectiveParams is of type 'UNSET'.
         */
        bool isUnset() const {
            return Type::UNSET == type;
        }

        /**
         * Return whether this DirectiveParams is of type 'HANDLE_IMMEDIATELY'.
         *
         * @return Whether this DirectiveParams is of type 'HANDLE_IMMEDIATELY'.
         */
        bool isHandleImmediately() const {
            return Type::HANDLE_IMMEDIATELY == type;
        }

        /**
         * Return whether this DirectiveParams is of type 'PREHANDLE'.
         *
         * @return Whether this DirectiveParams is of type 'PREHANDLE'.
         */
        bool isPreHandle() const {
            return Type::PREHANDLE == type;
        }

        /**
         * Return whether this DirectiveParams is of type 'HANDLE'.
         *
         * @return Whether this DirectiveParams is of type 'HANDLE'.
         */
        bool isHandle() const {
            return Type::HANDLE == type;
        }

        /**
         * Return whether this DirectiveParams is of type 'CANCEL'.
         *
         * @return Whether this DirectiveParams is of type 'CANCEL'.
         */
        bool isCancel() const {
            return Type::CANCEL == type;
        }


        /**
         * Return whether this DirectiveParams is of type 'EXCEPTION'.
         *
         * @return Whether this DirectiveParams is of type 'EXCEPTION'.
         */
        bool isException() const {
            return Type::EXCEPTION == type;
        }

        /**
         * Return whether this DirectiveParams is of type 'TIMEOUT'.
         *
         * @return Whether this DirectiveParams is of type 'TIMEOUT'.
         */
        bool isTimeout() const {
            return Type::TIMEOUT == type;
        }

        // Enum for the way the directive was passed to DirectiveHandler.
        enum class Type {
            // Not yet set.
            UNSET,
            // Set when handleDirectiveImmediately is called.
            HANDLE_IMMEDIATELY,
            // Set when preHandleDirective is called.
            PREHANDLE,
            // Set when handleDirective is called.
            HANDLE,
            //Set when cancelDirective is called.
            CANCEL,
            // Set when sendExceptionEncountered is called.
            EXCEPTION,
            // Set when waitForNext times out waiting for a directive.
            TIMEOUT
        };

        // Type of how the directive was passed to DirectiveHandler.
        Type type;
        // AVSDirective passed from the Directive Sequencer to the DirectiveHandler. 
        std::shared_ptr<avsCommon::AVSDirective> directive;
        // DirectiveHandlerResult to inform the Directive Sequencer a directive has either successfully or
        // unsuccessfully handled.
        std::shared_ptr<DirectiveHandlerResultInterface> result;
        // Unparsed directive string passed to sendExceptionEncountered.
        std::string exceptionUnparsedDirective;
        // Error type passed to sendExceptionEncountered.
        ExceptionErrorType exceptionError;
        // Additional information passed to sendExceptionEncountered.
        std::string exceptionMessage;
    };

    /**
     * Function to retrieve the next DirectiveParams in the test queue or time out if the queue is empty. Takes a duration in seconds 
     * to wait before timing out.
     */
    DirectiveParams waitForNext(const std::chrono::seconds duration);

private:
    /// Mutex to protect m_queue.
    std::mutex m_mutex;
    /// Trigger to wake up waitForNext calls.
    std::condition_variable m_wakeTrigger;
    /// Queue of received directives that have not been waited on.
    std::deque<DirectiveParams> m_queue;
    /// map of message IDs to result handlers.
    std::unordered_map<std::string, std::shared_ptr<DirectiveHandlerResultInterface>> results;
    /// map of message IDs to result handlers.
    std::unordered_map<std::string, std::shared_ptr<avsCommon::AVSDirective>> directives;
};


class AudioInputProcessorTest : public ::testing::Test {
protected:
    void SetUp() override {

        std::ifstream infile(configPath);
        ASSERT_TRUE(infile.good());
        ASSERT_TRUE(AlexaClientSDKInit::initialize({&infile}));
        m_authObserver = std::make_shared<AuthObserver>();
        m_authDelegate = AuthDelegate::create();
        m_authDelegate->setAuthObserver(m_authObserver);
        m_connectionStatusObserver = std::make_shared<ConnectionStatusObserver>();

        auto attachmentManager = std::make_shared<AttachmentManager>(AttachmentManager::AttachmentType::IN_PROCESS);
        bool isEnabled = false;
        m_messageRouter = std::make_shared<HTTP2MessageRouter>(m_authDelegate, attachmentManager);
        m_directiveHandler = std::make_shared<TestDirectiveHandler>();
        m_directiveSequencer = DirectiveSequencer::create(m_directiveHandler);
        ASSERT_NE(nullptr, m_directiveSequencer);
        m_messageInterpreter = std::make_shared<MessageInterpreter>(
            m_directiveHandler,
            m_directiveSequencer,
            attachmentManager
            );

        m_compatibleAudioFormat.sampleRateHz = COMPATIBLE_SAMPLE_RATE;
        m_compatibleAudioFormat.sampleSizeInBits = COMPATIBLE_SAMPLE_SIZE_IN_BITS;
        m_compatibleAudioFormat.numChannels = COMPATIBLE_NUM_CHANNELS;
        m_compatibleAudioFormat.endianness = COMPATIBLE_ENDIANNESS;
        m_compatibleAudioFormat.encoding = COMPATIBLE_ENCODING;

        size_t nWords = 1024*1024;
        size_t wordSize = 2;
        size_t maxReaders = 3;
        size_t bufferSize = AudioInputStream::calculateBufferSize(nWords, wordSize, maxReaders);

        auto m_Buffer = std::make_shared<avsCommon::sdkInterfaces::AudioInputStream::Buffer>(bufferSize);
        auto m_Sds = avsCommon::sdkInterfaces::AudioInputStream::create(m_Buffer, wordSize, maxReaders);
        ASSERT_NE (nullptr, m_Sds);
        m_AudioBuffer = std::move(m_Sds);
        m_AudioBufferWriter = m_AudioBuffer->createWriter(
            avsCommon::sdkInterfaces::AudioInputStream::Writer::Policy::NONBLOCKABLE);
        ASSERT_NE (nullptr, m_AudioBufferWriter);


        
        // Set up tap and hold to talk buttons.
        bool alwaysReadable = true;
        bool canOverride = true;
        bool canBeOverridden = true;
        m_HoldToTalkAudioProvider = std::make_shared<AudioProvider>( m_AudioBuffer, m_compatibleAudioFormat,
            ASRProfile::CLOSE_TALK, !alwaysReadable, canOverride, !canBeOverridden);
        m_TapToTalkAudioProvider = std::make_shared<AudioProvider>( m_AudioBuffer, m_compatibleAudioFormat,
            ASRProfile::NEAR_FIELD, alwaysReadable, canOverride, !canBeOverridden);

        m_tapToTalkButton = std::make_shared<tapToTalkButton>();
        m_holdToTalkButton = std::make_shared<holdToTalkButton>();

        // Set up Focus Manager 
        FocusManager::ChannelConfiguration dialogChannelConfig{DIALOG_CHANNEL_NAME, DIALOG_CHANNEL_PRIORITY};
        FocusManager::ChannelConfiguration alertsChannelConfig{ALERTS_CHANNEL_NAME, ALERTS_CHANNEL_PRIORITY};
        FocusManager::ChannelConfiguration contentChannelConfig{CONTENT_CHANNEL_NAME, CONTENT_CHANNEL_PRIORITY};
        std::vector<FocusManager::ChannelConfiguration> channelConfigurations {
            dialogChannelConfig, alertsChannelConfig, contentChannelConfig
        };
        m_focusManager = std::make_shared<FocusManager>(channelConfigurations);

        m_contextManager = ContextManager::create();
        ASSERT_NE (nullptr, m_contextManager);

        m_stateProvider = std::make_shared<testStateProvider>(m_contextManager);
        m_contextManager->setStateProvider(VOLUME_STATE_PAIR, m_stateProvider);

        // Set up connection and connect
        m_avsConnectionManager = std::make_shared<TestMessageSender>(
                m_messageRouter,
                isEnabled,
                m_connectionStatusObserver,
                m_messageInterpreter);
                ASSERT_NE (nullptr, m_avsConnectionManager);
        connect();

        m_AudioInputProcessor = AudioInputProcessor::create(
            m_directiveSequencer,
            m_avsConnectionManager, 
            m_contextManager, 
            m_focusManager, 
            m_directiveHandler
        );
        ASSERT_NE (nullptr, m_AudioInputProcessor);

        m_testClient = std::make_shared<TestClient>();

        m_StateObserver = std::make_shared<AipStateObserver>();
        ASSERT_NE(nullptr, m_StateObserver); 
        m_AudioInputProcessor->addObserver(m_StateObserver);

        auto aipConfig = m_AudioInputProcessor->getConfiguration();
        ASSERT_TRUE(m_directiveSequencer->addDirectiveHandlers(aipConfig));

#ifdef KWD
        m_wakeWordTrigger = std::make_shared<wakeWordTrigger>(m_compatibleAudioFormat, m_AudioInputProcessor);

#ifdef KWD_KITTAI
        kwd::KittAiKeyWordDetector::KittAiConfiguration config;
        config = {inputPath+MODEL_FILE, MODEL_KEYWORD, KITTAI_SENSITIVITY};
        m_detector = kwd::KittAiKeyWordDetector::create(
                m_AudioBuffer, 
                m_compatibleAudioFormat, 
                {m_wakeWordTrigger}, 
                {}, 
                inputPath+RESOURCE_FILE, 
                {config}, 
                2.0, 
                false);
        ASSERT_TRUE(m_detector); 
#endif 
#endif
        ASSERT_TRUE(registerHandler(SET_MUTE_PAIR, BlockingPolicy::NON_BLOCKING, m_directiveHandler));
        ASSERT_TRUE(registerHandler(SPEAK_PAIR, BlockingPolicy::BLOCKING, m_directiveHandler));
    }

        void TearDown() override {
        disconnect();
        m_AudioInputProcessor->resetState().wait();
        m_directiveSequencer->shutdown();
        AlexaClientSDKInit::uninitialize();
    }

    /**
     * Connect to AVS.
     */
    void connect() {
        ASSERT_TRUE(m_authObserver->waitFor(AuthObserver::State::REFRESHED))
            << "Retrieving the auth token timed out.";
        m_avsConnectionManager->enable();
        ASSERT_TRUE(m_connectionStatusObserver->waitFor(ConnectionStatus::CONNECTED))
            << "Connecting timed out.";
    }

    /**
     * Disconnect from AVS.
     */
    void disconnect() {
        m_avsConnectionManager->disable();
        ASSERT_TRUE(m_connectionStatusObserver->waitFor(ConnectionStatus::DISCONNECTED)) 
            << "Connecting timed out.";
    }

    /**
     * Register @c m_handler as the handler for the specified directive, using the specified policy.
     *
     * @param namespaceAndName The type of directive m_handler will handle.
     * @param blockingPolicy The blocking policy to be applied when handling directives of the specified type.
     * @return Whether the handler was registered.
     */
    bool registerHandler(
            const NamespaceAndName& namespaceAndName,
            BlockingPolicy blockingPolicy) {
        return registerHandler(namespaceAndName, blockingPolicy, m_directiveHandler);
    }

    /**
     * Register a handler for the specified directive, using the specified policy.
     *
     * @param namespaceAndName The type of directive m_handler will handle.
     * @param blockingPolicy The blocking policy to be applied when handling directives of the specified type.
     * @param handler The handler for the handle the specified type of directive.
     * @return Whether the handler was registered.
     */
    bool registerHandler(
            const NamespaceAndName& namespaceAndName,
            BlockingPolicy blockingPolicy,
            std::shared_ptr<TestDirectiveHandler> handler) {
        return m_directiveSequencer->addDirectiveHandlers({
                {{namespaceAndName.nameSpace, namespaceAndName.name}, {handler, blockingPolicy}}
        });
    }

    bool checkSentEventName(std::shared_ptr<TestMessageSender> connectionManager, std::string expectedName){
        TestMessageSender::SendParams sendParams = connectionManager->waitForNext(AUDIO_FILE_TIMEOUT_DURATION);
        if (TestMessageSender::SendParams::Type::SEND == sendParams.type) {
            std::string eventString;
            std::string eventHeader;
            std::string eventName;
            jsonUtils::lookupStringValue(sendParams.request->getJsonContent(), "event", &eventString);
            jsonUtils::lookupStringValue(eventString, "header", &eventHeader);
            jsonUtils::lookupStringValue(eventHeader, "name", &eventName);
            if(eventName == expectedName) {
                return true; 
            }
            else {
                return false;
            }    
        }
        else {
            return false;
        }
    }

    std::shared_ptr<AuthObserver> m_authObserver;
    std::shared_ptr<AuthDelegate> m_authDelegate;
    std::shared_ptr<ConnectionStatusObserver> m_connectionStatusObserver;
    std::shared_ptr<MessageRouter> m_messageRouter;
    std::shared_ptr<TestMessageSender> m_avsConnectionManager;
    std::shared_ptr<TestDirectiveHandler> m_directiveHandler;
    std::shared_ptr<DirectiveSequencerInterface> m_directiveSequencer;
    std::shared_ptr<MessageInterpreter> m_messageInterpreter;
    std::shared_ptr<avsCommon::sdkInterfaces::ContextManagerInterface> m_contextManager;
    std::shared_ptr<afml::FocusManager> m_focusManager;
    std::shared_ptr<TestClient> m_testClient;
    std::shared_ptr<AudioInputProcessor> m_AudioInputProcessor;
    std::shared_ptr<AipStateObserver> m_StateObserver;
    std::shared_ptr<tapToTalkButton> m_tapToTalkButton;
    std::shared_ptr<holdToTalkButton> m_holdToTalkButton;
    std::shared_ptr<testStateProvider> m_stateProvider;
    std::unique_ptr<AudioInputStream::Writer> m_AudioBufferWriter;
    std::shared_ptr<AudioInputStream> m_AudioBuffer;
    std::shared_ptr<AudioProvider> m_TapToTalkAudioProvider;
    std::shared_ptr<AudioProvider> m_HoldToTalkAudioProvider;
#ifdef KWD
    std::shared_ptr<wakeWordTrigger> m_wakeWordTrigger;
#endif
    AudioFormat m_compatibleAudioFormat;
#ifdef KWD_KITTAI
    std::unique_ptr<kwd::KittAiKeyWordDetector> m_detector;
#endif
};

/**
 * Parse an @c AVSDirective from a JSON string.
 *
 * @param rawJSON The JSON to parse.
 * @param attachmentManager The @c AttachmentManager to initialize the new @c AVSDirective with.
 * @return A new @c AVSDirective, or nullptr if parsing the JSON fails.
 */
static std::shared_ptr<AVSDirective> parseDirective(
        const std::string& rawJSON, std::shared_ptr<AttachmentManager> attachmentManager) {

    std::string directiveJSON;
    std::string headerJSON;
    std::string payloadJSON;
    std::string nameSpace;
    std::string name;
    std::string messageId;
    std::string dialogRequestId;

    if (!jsonUtils::lookupStringValue(rawJSON, JSON_MESSAGE_DIRECTIVE_KEY, &directiveJSON) ||
            !jsonUtils::lookupStringValue(directiveJSON, JSON_MESSAGE_HEADER_KEY, &headerJSON) ||
            !jsonUtils::lookupStringValue(directiveJSON, JSON_MESSAGE_PAYLOAD_KEY, &payloadJSON) ||
            !jsonUtils::lookupStringValue(headerJSON, JSON_MESSAGE_NAMESPACE_KEY, &nameSpace) ||
            !jsonUtils::lookupStringValue(headerJSON, JSON_MESSAGE_NAME_KEY, &name) ||
            !jsonUtils::lookupStringValue(headerJSON, JSON_MESSAGE_MESSAGE_ID_KEY, &messageId)) {
        ACSDK_ERROR(LX("parseDirectiveFailed").d("rawJSON", rawJSON));
        return nullptr;
    }

    jsonUtils::lookupStringValue(headerJSON, JSON_MESSAGE_NAMESPACE_KEY, &dialogRequestId);

    auto header = std::make_shared<AVSMessageHeader>(nameSpace, name, messageId, dialogRequestId);
    return AVSDirective::create(rawJSON, header, payloadJSON, attachmentManager, "");
}


void TestDirectiveHandler::handleDirectiveImmediately(std::shared_ptr<avsCommon::AVSDirective> directive) {
    std::unique_lock<std::mutex> lock(m_mutex);
    TestDirectiveHandler::DirectiveParams dp;
    dp.type = DirectiveParams::Type::HANDLE_IMMEDIATELY;
    dp.directive = directive;
    m_queue.push_back(dp);
    m_wakeTrigger.notify_all();
}

void TestDirectiveHandler::preHandleDirective(
            std::shared_ptr<avsCommon::AVSDirective> directive,
            std::unique_ptr<DirectiveHandlerResultInterface> result)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    TestDirectiveHandler::DirectiveParams dp;
    dp.type = TestDirectiveHandler::DirectiveParams::Type::PREHANDLE;
    dp.directive = directive;
    dp.result = std::move(result);
    ASSERT_EQ(results.find(directive->getMessageId()), results.end());
    results[directive->getMessageId()] = dp.result;
    ASSERT_EQ(directives.find(directive->getMessageId()), directives.end());
    directives[directive->getMessageId()] = directive;
    m_queue.push_back(dp);
    m_wakeTrigger.notify_all();
}

bool TestDirectiveHandler::handleDirective(const std::string& messageId) {
    std::unique_lock<std::mutex> lock(m_mutex);
    TestDirectiveHandler::DirectiveParams dp;
    dp.type = DirectiveParams::Type::HANDLE;
    auto result = results.find(messageId);
    if (results.end() == result) {
        EXPECT_NE(result, results.end());
        return false;
    }
    dp.result = result->second;
    auto directive = directives.find(messageId);
    if (directives.end() == directive) {
        EXPECT_NE(directive, directives.end());
        return false;
    }
    dp.directive = directive->second;
    m_queue.push_back(dp);
    m_wakeTrigger.notify_all();
    return true;
}

void TestDirectiveHandler::cancelDirective(const std::string& messageId) {
    std::unique_lock<std::mutex> lock(m_mutex);
    TestDirectiveHandler::DirectiveParams dp;
    dp.type = DirectiveParams::Type::CANCEL;
    auto result = results.find(messageId);
    ASSERT_NE(result, results.end());
    dp.result = result->second;
    results.erase(result);
    auto directive = directives.find(messageId);
    ASSERT_NE(directive, directives.end());
    dp.directive = directive->second;
    directives.erase(directive);
    m_queue.push_back(dp);
    m_wakeTrigger.notify_all();
}

void TestDirectiveHandler::onDeregistered() {
}

void TestDirectiveHandler::sendExceptionEncountered(
    const std::string& unparsedDirective,
    ExceptionErrorType error,
    const std::string& message) {

    ACSDK_INFO(LX("sendExceptionEncountered").d("unparsed", unparsedDirective).d("error", error).d("message", message));

    std::unique_lock<std::mutex> lock(m_mutex);
    TestDirectiveHandler::DirectiveParams dp;
    dp.type = TestDirectiveHandler::DirectiveParams::Type::EXCEPTION;
    dp.directive = parseDirective(
            unparsedDirective,
            std::make_shared<AttachmentManager>(AttachmentManager::AttachmentType::IN_PROCESS));
    dp.exceptionUnparsedDirective = unparsedDirective;
    dp.exceptionError = error;
    dp.exceptionMessage = message;
    m_queue.push_back(dp);
    m_wakeTrigger.notify_all();
}

TestDirectiveHandler::DirectiveParams TestDirectiveHandler::waitForNext(const std::chrono::seconds duration) {
    DirectiveParams ret;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_wakeTrigger.wait_for(lock, duration, [this]() { return !m_queue.empty(); })) {
        ret.type = DirectiveParams::Type::TIMEOUT;
        return ret;
    }
    ret = m_queue.front();
    m_queue.pop_front();
    return ret;
}

TestDirectiveHandler::DirectiveParams::DirectiveParams() : type{Type::UNSET} {
}



std::vector<int16_t> readAudioFromFile(const std::string &fileName, bool* errorOccurred) {
    const int RIFF_HEADER_SIZE = 44;

    std::ifstream inputFile(fileName.c_str(), std::ifstream::binary);
    if (!inputFile.good()) {
        std::cout << "Couldn't open audio file!" << std::endl;
        if (errorOccurred) {
            *errorOccurred = true;
        }
        return {};
    }
    inputFile.seekg(0, std::ios::end);
    int fileLengthInBytes = inputFile.tellg();
    if (fileLengthInBytes <= RIFF_HEADER_SIZE) {
        std::cout << "File should be larger than 44 bytes, which is the size of the RIFF header" << std::endl;
        if (errorOccurred) {
            *errorOccurred = true;
        }
        return {};
    }

    inputFile.seekg(RIFF_HEADER_SIZE, std::ios::beg);

    int numSamples = (fileLengthInBytes - RIFF_HEADER_SIZE) / 2;

    std::vector<int16_t> retVal(numSamples, 0);

    inputFile.read((char *)&retVal[0], numSamples * 2);

    if (inputFile.gcount() != numSamples*2) {
        std::cout << "Error reading audio file" << std::endl;
        if (errorOccurred) {
            *errorOccurred = true;
        }
        return {};
    }

    inputFile.close();
    if (errorOccurred) {
        *errorOccurred = false;
    }
    return retVal;
}

/**
 * Test AudioInputProcessor's ability to handle a simple interation triggered by a wakeword.
 *
 * To do this, audio of "Alexa, tell me a joke" is fed into a stream that is being read by a wake word engine. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute and Speak directive.
 */
 #ifdef KWD
 TEST_F(AudioInputProcessorTest, wakeWordJoke) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Put audio onto the SDS saying "Alexa, Tell me a joke".
    bool error;
    std::string file = inputPath + ALEXA_JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that prehandle and handle for setMute and Speak has reached the test SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(std::chrono::seconds(AUDIO_FILE_TIMEOUT_DURATION));
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }
}
#endif

/**
 * Test AudioInputProcessor's ability to handle a recognize triggered by a wakeword followed by silence .
 *
 * To do this, audio of "Alexa, ........." is fed into a stream that is being read by a wake word engine. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with no directives.
 */
 #ifdef KWD
TEST_F(AudioInputProcessorTest, wakeWordSilence) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Put audio onto the SDS saying "Alexa ......".
    bool error;
    std::string file = inputPath + ALEXA_SILENCE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // Check that a recognize event was sent 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that no prehandle or handle for setMute and Speak has reached the test SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}
#endif

/**
 * Test AudioInputProcessor's ability to handle a multiturn interation triggered by a wakeword.
 *
 * To do this, audio of "Alexa, wikipedia" is fed into a stream that is being read by a wake word engine. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute, Speak, 
 * and ExpectSpeech directive. Audio of "Lions" is then fed into the stream and another recognize event is sent.
 */
 #ifdef KWD
TEST_F(AudioInputProcessorTest, wakeWordMultiturn) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Put audio onto the SDS saying "Alexa, wikipedia".
    bool error;
    std::string file = inputPath + ALEXA_WIKI_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that prehandle and handle for setMute and Speak has reached the test SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(std::chrono::seconds(AUDIO_FILE_TIMEOUT_DURATION));
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is now in EXPECTING_SPEECH state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::EXPECTING_SPEECH, AUDIO_FILE_TIMEOUT_DURATION));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Lions".
    bool secondError;
    std::string secondFile = inputPath + LIONS_AUDIO_FILE;
    std::vector<int16_t> secondAudioData = readAudioFromFile(secondFile, &secondError);
    ASSERT_FALSE(secondError);
    m_AudioBufferWriter->write(secondAudioData.data(), secondAudioData.size());

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that prehandle and handle for setMute and Speak has reached the test SS.
    params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);

   // params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }
}
#endif

/**
 * Test AudioInputProcessor's ability to handle a simple interation triggered by a wakeword but no user response.
 *
 * To do this, audio of "Alexa, wikipedia" is fed into a stream that is being read by a wake word engine. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute, Speak, 
 * and ExpectSpeech directive. Audio of "...." is then fed into the stream and another recognize event is sent 
 * but no directives are given in response.
 */
 #ifdef KWD
TEST_F(AudioInputProcessorTest, wakeWordMultiturnWithoutUserResponse) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Put audio onto the SDS saying "Alexa, wikipedia".
    bool error;
    std::string file = inputPath + ALEXA_WIKI_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // Check that a recognize event was sent 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that prehandle and handle for setMute and Speak has reached the test SS 
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(std::chrono::seconds(AUDIO_FILE_TIMEOUT_DURATION));
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }

    // Check that AIP is now in EXPECTING_SPEECH state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::EXPECTING_SPEECH, AUDIO_FILE_TIMEOUT_DURATION));

    // Put audio onto the SDS saying ".......".
    bool secondError;
    std::string secondFile = inputPath + SILENCE_AUDIO_FILE;
    std::vector<int16_t> secondAudioData = readAudioFromFile(secondFile, &secondError);
    ASSERT_FALSE(secondError);
    m_AudioBufferWriter->write(secondAudioData.data(), secondAudioData.size());

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was not sent.
    ASSERT_FALSE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_FALSE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(WANTING_TIMEOUT_DURATION), FocusState::NONE);
}
#endif

/**
 * Test AudioInputProcessor's ability to handle a simple interation triggered by a tap to talk button.
 *
 * To do this, audio of "Tell me a joke" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute and Speak directive.
 */
TEST_F(AudioInputProcessorTest, tapToTalkJoke) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_tapToTalkButton->startRecognizing(m_AudioInputProcessor, m_TapToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Tell me a joke".
    bool error;
    std::string file = inputPath + JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }
}

/**
 * Test AudioInputProcessor's ability to handle a silent interation triggered by a tap to talk button.
 *
 * To do this, audio of "....." is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds no directives.
 */
TEST_F(AudioInputProcessorTest, tapToTalkSilence) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_tapToTalkButton->startRecognizing(m_AudioInputProcessor, m_TapToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying ".......".
    bool error;
    std::string file = inputPath + SILENCE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that no directives arrived to the fake SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}

/**
 * Test AudioInputProcessor's ability to handle no audio being written triggered by a tap to talk button.
 *
 * To do this, no audio is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with no directive.
 */
TEST_F(AudioInputProcessorTest, tapToTalkNoAudio) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_tapToTalkButton->startRecognizing(m_AudioInputProcessor, m_TapToTalkAudioProvider));

    // Put no audio onto the SDS.

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that a recognize event was sent 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    //Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has not changed.
    ASSERT_EQ(m_testClient->waitForFocusChange(WANTING_TIMEOUT_DURATION), FocusState::NONE);

    // Check that no directives arrived to the fake SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}

/**
 * Test AudioInputProcessor's ability to handle an interation triggered by a tap to talk button with wake word.
 *
 * To do this, audio of "Alexa, Tell me a joke" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send only one Recognize event to AVS which responds with a SetMute and Speak directive.
 */
 #ifdef KWD
TEST_F(AudioInputProcessorTest, tapToTalkWithWakeWordConflict) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_tapToTalkButton->startRecognizing(m_AudioInputProcessor, m_TapToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Alexa, Tell me a joke".
    bool error;
    std::string file = inputPath + ALEXA_JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }
}
#endif

/**
 * Test AudioInputProcessor's ability to handle a multiturn interation triggered by a tap to talk button.
 *
 * To do this, audio of "Wikipedia" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute, Speak, 
 * and ExpectSpeech directive. Audio of "Lions" is then fed into the stream and another recognize event is sent.
 */
TEST_F(AudioInputProcessorTest, tapToTalkMultiturn) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_tapToTalkButton->startRecognizing(m_AudioInputProcessor, m_TapToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Wikipedia".
    bool error;
    std::string file = inputPath + WIKI_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent. 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS. 
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }

    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is now in EXPECTING_SPEECH state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::EXPECTING_SPEECH, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Lions".
    bool secondError;
    std::string secondFile = inputPath + LIONS_AUDIO_FILE;
    std::vector<int16_t> secondAudioData = readAudioFromFile(secondFile, &secondError);
    ASSERT_FALSE(secondError);
    m_AudioBufferWriter->write(secondAudioData.data(), secondAudioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS. 
    params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }
}

/**
 * Test AudioInputProcessor's ability to handle a multiturn interation triggered by a tap to talk button but no user response.
 *
 * To do this, audio of "Wikipedia" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute, Speak, 
 * and ExpectSpeech directive. Audio of "...." is then fed into the stream and another recognize event is sent 
 * but no directives are given in response.
 */
TEST_F(AudioInputProcessorTest, tapToTalkMultiturnWithoutUserResponse) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_tapToTalkButton->startRecognizing(m_AudioInputProcessor, m_TapToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Wikipedia".
    bool error;
    std::string file = inputPath + WIKI_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent. 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS. 
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is now in EXPECTING_SPEECH state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::EXPECTING_SPEECH, AUDIO_FILE_TIMEOUT_DURATION));

    // Put audio onto the SDS saying ".......".
    bool secondError;
    std::string secondFile = inputPath + SILENCE_AUDIO_FILE;
    std::vector<int16_t> secondAudioData = readAudioFromFile(secondFile, &secondError);
    ASSERT_FALSE(secondError);
    m_AudioBufferWriter->write(secondAudioData.data(), secondAudioData.size());

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that prehandle and handle for setMute and Speak has reached the test SS.
    params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);

    // Check that a recognize event was not sent.
    ASSERT_FALSE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_FALSE(m_stateProvider->checkStateRequested());

    // The test channel client has not changed.
    ASSERT_EQ(m_testClient->waitForFocusChange(WANTING_TIMEOUT_DURATION), FocusState::NONE);

    // Check that no directives arrived to the fake SS.
    params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}

/**
 * Test AudioInputProcessor's ability to handle a cancel partway through an interaction.
 *
 * To do this, audio of "Tell me a joke" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * button then sends a reset command and no recognize event is sent.
 */
TEST_F(AudioInputProcessorTest, tapToTalkCancel) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_tapToTalkButton->startRecognizing(m_AudioInputProcessor, m_TapToTalkAudioProvider));

    // Cancel the interaction.
    m_AudioInputProcessor->resetState();

    // Check that AIP was briefly in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Tell me a joke".
    bool error;
    std::string file = inputPath + JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // Check that a recognize event was sent.
    ASSERT_FALSE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that no directives arrived to the fake SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}

/**
 * Test AudioInputProcessor's ability to handle a simple interation triggered by a hold to talk button.
 *
 * To do this, audio of "Tell me a joke" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute and Speak directive.
 */
TEST_F(AudioInputProcessorTest, holdToTalkJoke) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_NE(nullptr, m_HoldToTalkAudioProvider);
    ASSERT_TRUE(m_holdToTalkButton->startRecognizing(m_AudioInputProcessor, m_HoldToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Tell me a joke".
    bool error;
    std::string file = inputPath + JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Stop holding the button.
    ASSERT_TRUE(m_holdToTalkButton->stopRecognizing(m_AudioInputProcessor));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }
}

/**
 * Test AudioInputProcessor's ability to handle a multiturn interation triggered by a hold to talk button.
 *
 * To do this, audio of "Wikipedia" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute, Speak, 
 * and ExpectSpeech directive. Audio of "Lions" is then fed into the stream and another recognize event is sent.
 */
TEST_F(AudioInputProcessorTest, holdToTalkMultiturn) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_holdToTalkButton->startRecognizing(m_AudioInputProcessor, m_HoldToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Tell me a joke".
    bool error;
    std::string file = inputPath + JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Stop holding the button.
    ASSERT_TRUE(m_holdToTalkButton->stopRecognizing(m_AudioInputProcessor));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent. 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS. 
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }

    // AIP was in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_holdToTalkButton->startRecognizing(m_AudioInputProcessor, m_HoldToTalkAudioProvider));

    // Put audio onto the SDS of "Lions".
    bool secondError;
    file = inputPath + LIONS_AUDIO_FILE;
    std::vector<int16_t> secondAudioData = readAudioFromFile(file, &secondError);
    ASSERT_FALSE(secondError);
    m_AudioBufferWriter->write(secondAudioData.data(), secondAudioData.size());

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Stop holding the button.
    ASSERT_TRUE(m_holdToTalkButton->stopRecognizing(m_AudioInputProcessor));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent. 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS. 
    params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }
}

/**
 * Test AudioInputProcessor's ability to handle a multiturn interation triggered by a hold to talk button but no user response.
 *
 * To do this, audio of "Wikipedia" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute, Speak, 
 * and ExpectSpeech directive. Audio of "...." is then fed into the stream and another recognize event is sent 
 * but no directives are given in response.
 */
TEST_F(AudioInputProcessorTest, holdToTalkMultiTurnWithSilence) {
      // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_holdToTalkButton->startRecognizing(m_AudioInputProcessor, m_HoldToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Tell me a joke".
    bool error;
    std::string file = inputPath + JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Stop holding the button.
    ASSERT_TRUE(m_holdToTalkButton->stopRecognizing(m_AudioInputProcessor));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent. 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS. 
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }

    // Check that AIP was in an IDLE state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_holdToTalkButton->startRecognizing(m_AudioInputProcessor, m_HoldToTalkAudioProvider));

    // Put audio onto the SDS of silence.
    bool secondError;
    file = inputPath + SILENCE_AUDIO_FILE;
    std::vector<int16_t> secondAudioData = readAudioFromFile(file, &secondError);
    ASSERT_FALSE(secondError);
    m_AudioBufferWriter->write(secondAudioData.data(), secondAudioData.size());

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Stop holding the button.
    ASSERT_TRUE(m_holdToTalkButton->stopRecognizing(m_AudioInputProcessor));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent. 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that no prehandle and handle for setMute and Speak has reached the test SS.
    params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}

/**
 * Test AudioInputProcessor's ability to handle a multiturn interation triggered by a hold to talk button that times out.
 *
 * To do this, audio of "Wikipedia" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with a SetMute, Speak, 
 * and ExpectSpeech directive. The button does not trigger another recognize so no recognize event is sent 
 * and no directives are given in response. ExpectSpeechTimedOut event is observed to be sent.
 */
TEST_F(AudioInputProcessorTest, holdToTalkMultiturnWithTimeOut) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_holdToTalkButton->startRecognizing(m_AudioInputProcessor, m_HoldToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Put audio onto the SDS saying "Wikipedia".
    bool error;
    std::string file = inputPath + WIKI_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Stop holding the button.
    ASSERT_TRUE(m_holdToTalkButton->stopRecognizing(m_AudioInputProcessor));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that prehandle and handle for setMute and Speak has reached the test SS.
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_NE(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
    while (params.type != TestDirectiveHandler::DirectiveParams::Type::TIMEOUT) {
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
                params.result->setCompleted();
        }
        params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    }

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Do not signal to the AIP to start recognizing.

    // Check that AIP is now in EXPECTING_SPEECH state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::EXPECTING_SPEECH, AUDIO_FILE_TIMEOUT_DURATION));

    // The test channel client has stayed foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(WANTING_TIMEOUT_DURATION), FocusState::NONE);

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that ExpectSpeechTimeOut event has been sent.
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_EXPECT_SPEECH_TIMED_OUT));
}

/**
 * Test AudioInputProcessor's ability to handle no audio being written triggered by a hold to talk button.
 *
 * To do this, no audio is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * AudioInputProcessor is then observed to send a Recognize event to AVS which responds with no directive.
 */
TEST_F(AudioInputProcessorTest, holdToTalkNoAudio) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_TRUE(m_holdToTalkButton->startRecognizing(m_AudioInputProcessor, m_HoldToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Write nothing to the SDS.

    // The test channel client has been notified the alarm channel has been backgrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::BACKGROUND);

    // Stop holding the button.
    ASSERT_TRUE(m_holdToTalkButton->stopRecognizing(m_AudioInputProcessor));

    // Check that AIP is in BUSY state. 
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::BUSY, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // The test channel client has been notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Check that a recognize event was sent 
    ASSERT_TRUE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that no prehandle or handle for setMute and Speak has reached the test SS. 
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}

/**
 * Test AudioInputProcessor's ability to handle a cancel partway through a hold to talk interaction.
 *
 * To do this, audio of "Tell me a joke" is fed into a stream after button sends recognize to AudioInputProcessor. The 
 * button then sends a cancel command and no recognize event is sent.
 */
 
TEST_F(AudioInputProcessorTest, holdToTalkCancel) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Signal to the AIP to start recognizing.
    ASSERT_NE(nullptr, m_HoldToTalkAudioProvider);
    ASSERT_TRUE(m_holdToTalkButton->startRecognizing(m_AudioInputProcessor, m_HoldToTalkAudioProvider));

    // Check that AIP is now in RECOGNIZING state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::RECOGNIZING, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Cancel the recognize.
    m_AudioInputProcessor->resetState();

    // Put audio onto the SDS saying "Tell me a joke".
    bool error;
    std::string file = inputPath + JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    ASSERT_FALSE(audioData.empty());
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // The test channel client has not changed.
    ASSERT_EQ(m_testClient->waitForFocusChange(WANTING_TIMEOUT_DURATION), FocusState::NONE);

    // Stop holding the button.
    ASSERT_FALSE(m_holdToTalkButton->stopRecognizing(m_AudioInputProcessor));

    // Check that AIP is in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was not asked to provide context for the event.
    ASSERT_TRUE(m_stateProvider->checkStateRequested());

    // Check that a recognize event was sent.
    ASSERT_FALSE(checkSentEventName(m_avsConnectionManager, NAME_RECOGNIZE));

    // Check that no prehandle or handle for setMute and Speak has reached the test SS. 
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}

/**
 * Test AudioInputProcessor's ability to not handle audio when no recognize occurs.
 *
 * To do this, audio of "Tell me a joke" is fed into a stream that is being read by a wake word engine. The 
 * lack of the wakeword or button-initiated recognize results in no recognize event being sent.
 */
TEST_F(AudioInputProcessorTest, audioWithoutAnyTrigger) {
    // Check that AIP is in an IDLE state before starting.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Request the alarm channel for the test channel client.
    ASSERT_TRUE(m_focusManager->acquireChannel(ALERTS_CHANNEL_NAME, m_testClient, ALARM_ACTIVITY_ID));
    ASSERT_EQ(m_testClient->waitForFocusChange(AUDIO_FILE_TIMEOUT_DURATION), FocusState::FOREGROUND);

    // Put audio onto the SDS saying "Tell me a joke" without a trigger.
    bool error;
    std::string file = inputPath + JOKE_AUDIO_FILE;
    std::vector<int16_t> audioData = readAudioFromFile(file, &error);
    ASSERT_FALSE(error);
    m_AudioBufferWriter->write(audioData.data(), audioData.size());

    // Check that AIP is still in an IDLE state.
    ASSERT_TRUE(m_StateObserver->checkState(AudioInputProcessor::State::IDLE, AUDIO_FILE_TIMEOUT_DURATION)); 

    // Check that the test context provider was not asked to provide context for the event.
    ASSERT_FALSE(m_stateProvider->checkStateRequested());

    // The test channel client has been not notified the alarm channel has been foregrounded.
    ASSERT_EQ(m_testClient->waitForFocusChange(WANTING_TIMEOUT_DURATION), FocusState::NONE);

    // Check that no prehandle or handle has reached the test SS. 
    TestDirectiveHandler::DirectiveParams params = m_directiveHandler->waitForNext(DIRECTIVE_TIMEOUT_DURATION);
    ASSERT_EQ(params.type, TestDirectiveHandler::DirectiveParams::Type::TIMEOUT);
}

} // namespace integration
} // namespace alexaClientSDK

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc < 3) {
        std::cerr << "USAGE: AudioInputProcessorTest <path_to_AlexaClientSDKConfig.json> <path_to_inputs_folder>"
                << std::endl;
        return 1;

    } else {
        alexaClientSDK::integration::configPath = std::string(argv[1]);
        alexaClientSDK::integration::inputPath = std::string(argv[2]);
        return RUN_ALL_TESTS();
    }
}


