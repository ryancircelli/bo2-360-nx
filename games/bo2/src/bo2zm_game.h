// Addresses that differ between the game's two programs. The Zombies program (default_mp.xex) is what app/ builds;
// the campaign build (campaign/, default.xex) compiles some of the same files with BO2_GAME_CAMPAIGN defined, and
// each address written as BO2_GAME_ADDR(zombies, campaign) then takes the campaign's value. Each campaign address was
// found as the function or global doing the same job as the Zombies one, matched by its instructions.

#pragma once

#if defined(BO2_GAME_CAMPAIGN)
#define BO2_GAME_ADDR(zombies, campaign) (campaign)
#else
#define BO2_GAME_ADDR(zombies, campaign) (zombies)
#endif
