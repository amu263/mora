#pragma once
#include "core.hpp"
#include <optional>

namespace mora {
    // A selectable music-agent persona. It carries its own address style, its own taste when
    // proposing music, and its own UI palette, so switching persona is visible on the homepage and
    // in the conversation rather than only inside the model prompt.
    struct Persona {
        std::string id;      // stable key stored in settings.json and used by the agent tool
        std::wstring name;
        std::wstring tagline;
        std::wstring voice;  // how the model should sound when this persona answers
        std::wstring taste;  // what this persona tends to propose
        std::wstring greeting;
        RGB accent, surface, muted, background;
        bool builtin = true;
    };

    // Built-in personas, in selector order. "amu" is the existing blue-haired homepage character.
    const std::vector<Persona>& builtinPersonas();
    const Persona* findBuiltinPersona(const std::string& id);

    // Custom personas the user created by talking to the agent. Bounded and validated on load.
    std::vector<Persona> loadCustomPersonas();
    bool saveCustomPersonas(const std::vector<Persona>& personas);
    // Validates every field, refuses duplicates and caps the list; returns the stored persona.
    Persona createCustomPersona(const std::string& id, const std::wstring& name,
                                const std::wstring& tagline, const std::wstring& voice,
                                const std::wstring& taste, const std::wstring& greeting);
    // Built-in first, then custom; a missing id falls back to the first built-in.
    std::vector<Persona> allPersonas();
    const Persona& resolvePersona(const std::string& id);

    // The model system prompt for one persona, including how the user asked to be addressed.
    std::wstring personaSystemPrompt(const Persona& persona, const std::wstring& userName);
    // The bounded "act on vague requests" rules appended to every persona prompt: the same
    // intent-to-operation mapping for built-ins and custom personas, fronted by one line in the
    // persona's own voice. Kept short on purpose, so the persona contract stays small.
    std::wstring personaIntentGuidance(const Persona& persona);
    // Short line the conversation shows while a model request is in flight. Every built-in carries
    // at least three lines and rotates through them by `index` (negative indices are wrapped); a
    // custom persona without its own corpus gets a deterministic fallback derived from its id, so
    // the same persona always says the same thing instead of a generic stock phrase.
    std::wstring personaWaitLine(const Persona& persona, int index);
    // Turns the persona's taste into a bounded search seed for music_recommend.
    std::wstring personaSearchSeed(const Persona& persona);

    // Persona avatar box. Every built-in avatar is 48x50, matching the original Amu sprite.
    inline constexpr int personaAvatarWidth = 48, personaAvatarHeight = 50;
    // Thumbnail art for a built-in persona. Custom personas have no drawn art and return {}.
    std::optional<RGB> personaAvatarPixel(const std::string& id, int x, int y);
    bool personaHasDrawnAvatar(const std::string& id);
    // A deterministic procedural face for a persona without drawn art, derived from its palette.
    std::optional<RGB> personaGeneratedPixel(const Persona& persona, int x, int y);
} // namespace mora
