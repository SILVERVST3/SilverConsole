// SilverConsole - aplicativo. Codigo original (MIT).
#include <JuceHeader.h>
#include "MainComponent.h"

class SilverConsoleApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "SilverConsole";
        o.folderName = "SilverConsole";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (o);

        window = std::make_unique<MainWindow> (properties);
        openFromCommandLine (commandLine);
    }

    void shutdown() override
    {
        window.reset();
        properties.closeFiles();
    }

    void systemRequestedQuit() override
    {
        if (window != nullptr) window->content()->requestQuit();
        else quit();
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (window != nullptr) window->toFront (true);
        openFromCommandLine (commandLine);
    }

private:
    void openFromCommandLine (const juce::String& cmd)
    {
        auto arg = cmd.trim().unquoted();
        if (arg.isNotEmpty() && juce::File::isAbsolutePath (arg) && window != nullptr)
            window->content()->openFile (juce::File (arg));
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::ApplicationProperties& props)
            : DocumentWindow ("SilverConsole", juce::Colour (0xff15181c), juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new sc::MainComponent (props), true);
            setResizable (true, false);
            setResizeLimits (960, 700, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            content()->grabKeyboardFocus();
        }
        sc::MainComponent* content() { return dynamic_cast<sc::MainComponent*> (getContentComponent()); }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    juce::ApplicationProperties properties;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (SilverConsoleApplication)
