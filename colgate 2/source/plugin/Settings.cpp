// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Settings.h"

#include <juce_data_structures/juce_data_structures.h>

namespace colgate
{

namespace
{
juce::PropertiesFile::Options fileOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Colgate";
    o.folderName = "Hiccup Audio";
    o.filenameSuffix = "settings";
    o.osxLibrarySubFolder = "Application Support";
    return o;
}
} // namespace

UserSettings UserSettings::load()
{
    UserSettings s;
    juce::PropertiesFile file (fileOptions());
    s.windowSizeIndex = juce::jlimit (0, 4, file.getIntValue ("windowSize", s.windowSizeIndex));
    s.displayRateIndex = juce::jlimit (0, 4, file.getIntValue ("displayRate", s.displayRateIndex));
    s.defaultScaleIndex = juce::jlimit (0, 5, file.getIntValue ("defaultScale", s.defaultScaleIndex));
    return s;
}

void UserSettings::save() const
{
    juce::PropertiesFile file (fileOptions());
    file.setValue ("windowSize", windowSizeIndex);
    file.setValue ("displayRate", displayRateIndex);
    file.setValue ("defaultScale", defaultScaleIndex);
    file.saveIfNeeded();
}

} // namespace colgate
