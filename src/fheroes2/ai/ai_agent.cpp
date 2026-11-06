/***************************************************************************
 *   External agent player for fheroes2                                    *
 *                                                                         *
 *   When <data dir>/agent/color names a computer-controlled color, that   *
 *   kingdom's turn is not played by the built-in AI. Instead the game     *
 *   writes the visible game state to agent/state.json and executes the    *
 *   commands it finds in agent/commands.txt, until an "end" command.      *
 *   Battles are still resolved by the built-in battle AI.                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ai_hero_action.h"
#include "ai_planner.h"
#include "army.h"
#include "army_troop.h"
#include "castle.h"
#include "color.h"
#include "game_mode.h"
#include "heroes.h"
#include "heroes_recruits.h"
#include "buildinginfo.h"
#include "game_interface.h"
#include "interface_status.h"
#include "kingdom.h"
#include "localevent.h"
#include "logging.h"
#include "maps.h"
#include "maps_tiles.h"
#include "maps_tiles_helper.h"
#include "monster.h"
#include "mp2.h"
#include "race.h"
#include "resource.h"
#include "route.h"
#include "system.h"
#include "world.h"

namespace
{
    std::string agentDir()
    {
        return System::concatPath( System::GetDataDirectory( "fheroes2" ), "agent" );
    }

    std::string agentPath( const char * name )
    {
        return System::concatPath( agentDir(), name );
    }

    const char * colorName( const PlayerColor color )
    {
        switch ( color ) {
        case PlayerColor::BLUE:
            return "blue";
        case PlayerColor::GREEN:
            return "green";
        case PlayerColor::RED:
            return "red";
        case PlayerColor::YELLOW:
            return "yellow";
        case PlayerColor::ORANGE:
            return "orange";
        case PlayerColor::PURPLE:
            return "purple";
        default:
            return "none";
        }
    }

    std::string trim( const std::string & s )
    {
        const size_t b = s.find_first_not_of( " \t\r\n" );
        if ( b == std::string::npos ) {
            return {};
        }
        const size_t e = s.find_last_not_of( " \t\r\n" );
        return s.substr( b, e - b + 1 );
    }

    std::string readFile( const std::string & path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in ) {
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void writeFileAtomic( const std::string & path, const std::string & content )
    {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out( tmp, std::ios::binary | std::ios::trunc );
            out << content;
        }
        std::rename( tmp.c_str(), path.c_str() );
    }

    std::string esc( const std::string & s )
    {
        std::string r;
        r.reserve( s.size() + 2 );
        r += '"';
        for ( const char c : s ) {
            switch ( c ) {
            case '"':
                r += "\\\"";
                break;
            case '\\':
                r += "\\\\";
                break;
            case '\n':
                r += "\\n";
                break;
            case '\r':
                break;
            case '\t':
                r += ' ';
                break;
            default:
                if ( static_cast<unsigned char>( c ) < 0x20 ) {
                    r += ' ';
                }
                else {
                    r += c;
                }
            }
        }
        r += '"';
        return r;
    }

    std::string safe( const char * s )
    {
        return s ? s : "";
    }

    // Several engine helpers return nullptr for unnamed things; never build a std::string from it.
    std::string esc( const char * s )
    {
        return esc( std::string( s ? s : "" ) );
    }

    std::string fundsJson( const Funds & f )
    {
        std::ostringstream o;
        o << "{\"gold\":" << f.gold << ",\"wood\":" << f.wood << ",\"ore\":" << f.ore << ",\"mercury\":" << f.mercury << ",\"sulfur\":" << f.sulfur
          << ",\"crystal\":" << f.crystal << ",\"gems\":" << f.gems << "}";
        return o.str();
    }

    std::string armyJson( const Army & army )
    {
        std::ostringstream o;
        o << "[";
        bool first = true;
        for ( size_t i = 0; i < army.Size(); ++i ) {
            const Troop * troop = army.GetTroop( i );
            if ( troop == nullptr || !troop->isValid() ) {
                continue;
            }
            if ( !first ) {
                o << ",";
            }
            first = false;
            o << "{\"slot\":" << i << ",\"monster_id\":" << troop->GetID() << ",\"name\":" << esc( troop->GetName() ) << ",\"count\":" << troop->GetCount()
              << ",\"level\":" << troop->GetMonsterLevel() << "}";
        }
        o << "]";
        return o.str();
    }

    const std::array<uint32_t, 12> & castleDwellings()
    {
        static const std::array<uint32_t, 12> dwellings{ DWELLING_MONSTER1, DWELLING_MONSTER2, DWELLING_UPGRADE2, DWELLING_MONSTER3, DWELLING_UPGRADE3, DWELLING_MONSTER4,
                                                         DWELLING_UPGRADE4, DWELLING_MONSTER5, DWELLING_UPGRADE5, DWELLING_MONSTER6, DWELLING_UPGRADE6, DWELLING_UPGRADE7 };
        return dwellings;
    }

    const char * buildingStatusName( const BuildingStatus status )
    {
        switch ( status ) {
        case BuildingStatus::ALLOW_BUILD:
            return "can_build";
        case BuildingStatus::NOT_TODAY:
            return "already_built_today";
        case BuildingStatus::ALREADY_BUILT:
            return "built";
        case BuildingStatus::NEED_CASTLE:
            return "needs_castle";
        case BuildingStatus::SHIPYARD_NOT_ALLOWED:
            return "no_sea_access";
        case BuildingStatus::REQUIRES_BUILD:
            return "requires_other_building";
        case BuildingStatus::LACK_RESOURCES:
            return "not_enough_resources";
        default:
            return "unavailable";
        }
    }

    char tileChar( const Maps::Tile & tile, const PlayerColor myColor )
    {
        const MP2::MapObjectType type = tile.getMainObjectType();

        switch ( type ) {
        case MP2::OBJ_HERO: {
            const Heroes * hero = tile.getHero();
            return ( hero && hero->GetColor() == myColor ) ? '@' : 'E';
        }
        case MP2::OBJ_CASTLE: {
            const Castle * castle = world.getCastleEntrance( Maps::GetPoint( tile.GetIndex() ) );
            return ( castle && castle->GetColor() == myColor ) ? 'K' : 'C';
        }
        case MP2::OBJ_MONSTER:
            return 'm';
        case MP2::OBJ_MINE:
        case MP2::OBJ_SAWMILL:
        case MP2::OBJ_ALCHEMIST_LAB:
            return 'M';
        case MP2::OBJ_RESOURCE:
            return 'r';
        case MP2::OBJ_TREASURE_CHEST:
            return 't';
        case MP2::OBJ_ARTIFACT:
            return 'a';
        case MP2::OBJ_NONE:
            return tile.isWater() ? '~' : '.';
        default:
            break;
        }

        if ( MP2::isInGameActionObject( type ) ) {
            return 'o';
        }

        if ( tile.isWater() ) {
            return '~';
        }

        // Non-action objects (trees, mountains, rocks, castle walls...) are obstacles, roads and decorations are not.
        return tile.isPassableFrom( Direction::CENTER, false, true, myColor ) ? '.' : '#';
    }

    Castle * findCastle( Kingdom & kingdom, const int x, const int y )
    {
        for ( Castle * castle : kingdom.GetCastles() ) {
            if ( castle && castle->GetCenter().x == x && castle->GetCenter().y == y ) {
                return castle;
            }
        }
        return nullptr;
    }

    Heroes * findHero( Kingdom & kingdom, const int id )
    {
        for ( Heroes * hero : kingdom.GetHeroes() ) {
            if ( hero && hero->GetID() == id ) {
                return hero;
            }
        }
        return nullptr;
    }
}

bool AI::Planner::isAgentControlled( const PlayerColor color )
{
    const std::string wanted = trim( readFile( agentPath( "color" ) ) );
    return !wanted.empty() && wanted == colorName( color );
}

std::string AI::Planner::agentStateJson( Kingdom & kingdom, const std::string & lastResults, const uint32_t seq )
{
    const PlayerColor myColor = kingdom.GetColor();
    const int32_t width = world.w();
    const int32_t height = world.h();

    std::ostringstream o;
    o << "{\n\"seq\":" << seq << ",\n\"status\":\"waiting_for_commands\",\n";
    o << "\"my_color\":" << esc( colorName( myColor ) ) << ",\n";
    o << "\"date\":{\"day\":" << world.GetDay() << ",\"week\":" << world.GetWeek() << ",\"month\":" << world.GetMonth() << "},\n";
    o << "\"funds\":" << fundsJson( kingdom.GetFunds() ) << ",\n";
    o << "\"income_per_day\":" << fundsJson( kingdom.GetIncome() ) << ",\n";
    o << "\"map_size\":[" << width << "," << height << "],\n";
    o << "\"last_results\":" << ( lastResults.empty() ? "[]" : lastResults ) << ",\n";

    // Heroes
    o << "\"heroes\":[";
    bool firstHero = true;
    for ( Heroes * hero : kingdom.GetHeroes() ) {
        if ( hero == nullptr || !hero->isActive() ) {
            continue;
        }
        if ( !firstHero ) {
            o << ",";
        }
        firstHero = false;
        const Castle * inCastle = hero->inCastle();
        o << "\n {\"id\":" << hero->GetID() << ",\"name\":" << esc( hero->GetName() ) << ",\"race\":" << esc( Race::String( hero->GetRace() ) )
          << ",\"x\":" << hero->GetCenter().x << ",\"y\":" << hero->GetCenter().y << ",\"level\":" << hero->GetLevel() << ",\"attack\":" << hero->GetAttack()
          << ",\"defense\":" << hero->GetDefense() << ",\"power\":" << hero->GetPower() << ",\"knowledge\":" << hero->GetKnowledge()
          << ",\"spell_points\":" << hero->GetSpellPoints() << ",\"move_points\":" << hero->GetMovePoints() << ",\"max_move_points\":" << hero->GetMaxMovePoints()
          << ",\"on_boat\":" << ( hero->isShipMaster() ? "true" : "false" ) << ",\"patrol_only\":" << ( hero->Modes( Heroes::PATROL ) ? "true" : "false" ) << ",\"army_strength\":" << static_cast<int64_t>( hero->GetArmy().GetStrength() )
          << ",\"in_castle\":" << ( inCastle ? esc( inCastle->GetName() ) : "null" ) << ",\"army\":" << armyJson( hero->GetArmy() ) << "}";
    }
    o << "],\n";

    // Castles
    o << "\"castles\":[";
    bool firstCastle = true;
    for ( Castle * castle : kingdom.GetCastles() ) {
        if ( castle == nullptr ) {
            continue;
        }
        if ( !firstCastle ) {
            o << ",";
        }
        firstCastle = false;

        o << "\n {\"name\":" << esc( castle->GetName() ) << ",\"x\":" << castle->GetCenter().x << ",\"y\":" << castle->GetCenter().y
          << ",\"race\":" << esc( Race::String( castle->GetRace() ) ) << ",\"is_castle\":" << ( castle->isCastle() ? "true" : "false" )
          << ",\"can_build_today\":" << ( castle->Modes( Castle::ALLOW_TO_BUILD_TODAY ) ? "true" : "false" );

        const Heroes * guest = castle->GetHero();
        o << ",\"guest_hero_id\":" << ( guest ? std::to_string( guest->GetID() ) : "null" );
        o << ",\"garrison\":" << armyJson( castle->GetArmy() );

        o << ",\"built\":[";
        bool first = true;
        for ( int bit = 0; bit < 32; ++bit ) {
            const uint32_t building = 1U << bit;
            if ( castle->isBuild( building ) ) {
                if ( !first ) {
                    o << ",";
                }
                first = false;
                o << esc( Castle::GetStringBuilding( building, castle->GetRace() ) );
            }
        }
        o << "],\"buildable\":[";
        first = true;
        for ( int bit = 0; bit < 32; ++bit ) {
            const uint32_t building = 1U << bit;
            if ( castle->isBuild( building ) ) {
                continue;
            }
            const BuildingStatus status = castle->CheckBuyBuilding( building );
            if ( status == BuildingStatus::UNKNOWN_COND || status == BuildingStatus::BUILD_DISABLE || status == BuildingStatus::UNKNOWN_UPGRADE
                 || status == BuildingStatus::ALREADY_BUILT ) {
                continue;
            }
            if ( !first ) {
                o << ",";
            }
            first = false;
            o << "{\"id\":" << building << ",\"name\":" << esc( Castle::GetStringBuilding( building, castle->GetRace() ) ) << ",\"status\":\""
              << buildingStatusName( status ) << "\",\"cost\":" << fundsJson( BuildingInfo::GetCost( building, castle->GetRace() ) ) << "}";
        }
        o << "],\"dwellings\":[";
        first = true;
        for ( const uint32_t dwelling : castleDwellings() ) {
            if ( !castle->isBuild( dwelling ) ) {
                continue;
            }
            const Monster monster( castle->GetRace(), castle->GetActualDwelling( dwelling ) );
            if ( !monster.isValid() ) {
                continue;
            }
            if ( !first ) {
                o << ",";
            }
            first = false;
            o << "{\"dwelling_id\":" << dwelling << ",\"monster\":" << esc( monster.GetName() ) << ",\"monster_id\":" << monster.GetID()
              << ",\"available\":" << castle->getMonstersInDwelling( dwelling ) << ",\"unit_cost\":" << fundsJson( monster.GetCost() )
              << ",\"affordable_now\":" << castle->getRecruitLimit( monster, kingdom.GetFunds() ) << "}";
        }
        o << "]}";
    }
    o << "],\n";

    // Heroes for hire
    {
        const Recruits & recruits = kingdom.GetRecruits();
        o << "\"heroes_for_hire\":[";
        const Heroes * candidates[2] = { recruits.GetHero1(), recruits.GetHero2() };
        bool first = true;
        for ( int i = 0; i < 2; ++i ) {
            if ( candidates[i] == nullptr ) {
                continue;
            }
            if ( !first ) {
                o << ",";
            }
            first = false;
            o << "{\"slot\":" << ( i + 1 ) << ",\"name\":" << esc( candidates[i]->GetName() ) << ",\"race\":" << esc( Race::String( candidates[i]->GetRace() ) )
              << ",\"attack\":" << candidates[i]->GetAttack() << ",\"defense\":" << candidates[i]->GetDefense() << ",\"power\":" << candidates[i]->GetPower()
              << ",\"knowledge\":" << candidates[i]->GetKnowledge() << "}";
        }
        o << "],\n";
    }

    // Visible objects with distances from each of my heroes
    std::vector<int32_t> objectTiles;
    for ( int32_t idx = 0; idx < width * height; ++idx ) {
        const Maps::Tile & tile = world.getTile( idx );
        if ( tile.isFog( myColor ) ) {
            continue;
        }
        if ( MP2::isInGameActionObject( tile.getMainObjectType() ) ) {
            objectTiles.push_back( idx );
        }
    }

    // Distances per hero: compute once per hero
    std::vector<std::pair<int, std::vector<uint32_t>>> heroDistances;
    for ( Heroes * hero : kingdom.GetHeroes() ) {
        if ( hero == nullptr || !hero->isActive() ) {
            continue;
        }
        _pathfinder.reEvaluateIfNeeded( *hero );
        std::vector<uint32_t> distances;
        distances.reserve( objectTiles.size() );
        for ( const int32_t idx : objectTiles ) {
            distances.push_back( _pathfinder.getDistance( idx ) );
        }
        heroDistances.emplace_back( hero->GetID(), std::move( distances ) );
    }

    o << "\"visible_objects\":[";
    for ( size_t i = 0; i < objectTiles.size(); ++i ) {
        const int32_t idx = objectTiles[i];
        const Maps::Tile & tile = world.getTile( idx );
        const MP2::MapObjectType type = tile.getMainObjectType();
        const fheroes2::Point pt = Maps::GetPoint( idx );

        if ( i > 0 ) {
            o << ",";
        }
        o << "\n {\"x\":" << pt.x << ",\"y\":" << pt.y << ",\"type\":" << esc( MP2::StringObject( type ) );

        if ( type == MP2::OBJ_MONSTER ) {
            const Monster monster = Maps::getMonsterFromTile( tile );
            o << ",\"monster\":" << esc( monster.GetName() ) << ",\"count\":" << Maps::getMonsterCountFromTile( tile ) << ",\"strength\":" << static_cast<int64_t>( getTileArmyStrength( tile ) );
        }
        else if ( type == MP2::OBJ_HERO ) {
            const Heroes * other = tile.getHero();
            if ( other ) {
                o << ",\"hero\":" << esc( other->GetName() ) << ",\"color\":" << esc( colorName( other->GetColor() ) );
                if ( other->GetColor() != myColor ) {
                    o << ",\"army_strength\":" << static_cast<int64_t>( other->GetArmy().GetStrength() );
                }
            }
        }
        else if ( type == MP2::OBJ_CASTLE ) {
            const Castle * castle = world.getCastleEntrance( pt );
            if ( castle ) {
                o << ",\"castle\":" << esc( castle->GetName() ) << ",\"color\":" << esc( colorName( castle->GetColor() ) ) << ",\"is_castle\":"
                  << ( castle->isCastle() ? "true" : "false" );
            }
        }
        else if ( MP2::isCaptureObject( type ) ) {
            o << ",\"owner\":" << esc( colorName( Maps::getColorFromTile( tile ) ) );
        }

        o << ",\"move_cost\":{";
        for ( size_t h = 0; h < heroDistances.size(); ++h ) {
            if ( h > 0 ) {
                o << ",";
            }
            const uint32_t d = heroDistances[h].second[i];
            o << "\"" << heroDistances[h].first << "\":";
            if ( d == 0 ) {
                o << "null";
            }
            else {
                o << d;
            }
        }
        o << "}}";
    }
    o << "],\n";

    // ASCII map of what this kingdom can see
    o << "\"map_legend\":\"' '=fog '.'=open ground '#'=obstacle '~'=water '@'=my hero 'E'=other hero 'K'=my castle 'C'=other castle 'm'=monster 'M'=mine/sawmill/alchemist 'r'=resource 't'=treasure chest 'a'=artifact 'o'=other object. Row index = y, column = x. move_cost null = unreachable now or guarded by an army that is too strong for that hero. Moving next to a monster makes it attack.\",\n";
    o << "\"map\":[";
    for ( int32_t y = 0; y < height; ++y ) {
        std::string row;
        row.reserve( width );
        for ( int32_t x = 0; x < width; ++x ) {
            const Maps::Tile & tile = world.getTile( x, y );
            row += tile.isFog( myColor ) ? ' ' : tileChar( tile, myColor );
        }
        if ( y > 0 ) {
            o << ",";
        }
        o << "\n " << esc( row );
    }
    o << "],\n";

    o << "\"commands_help\":[\"move <hero_id> <x> <y>\",\"build <castle_x> <castle_y> <building_id>\",\"recruit <castle_x> <castle_y> <dwelling_id> <count|max>\","
         "\"upgrade <castle_x> <castle_y>\",\"hire <castle_x> <castle_y> <1|2>\",\"take_garrison <castle_x> <castle_y>\",\"leave_troop <hero_id> <slot>\","
         "\"wait\",\"end\"]\n";
    o << "}\n";
    return o.str();
}

fheroes2::GameMode AI::Planner::AgentKingdomTurn( Kingdom & kingdom )
{
    const PlayerColor myColor = kingdom.GetColor();

    if ( kingdom.isLoss() || myColor == PlayerColor::NONE ) {
        kingdom.LossPostActions();
        return fheroes2::GameMode::END_TURN;
    }

    // Prepare the same caches as the regular AI turn so the shared hero action code behaves.
    _mapActionObjects.clear();
    _priorityTargets.clear();
    _enemyArmies.clear();
    _tileArmyStrengthValues.clear();
    _regions.clear();
    _regions.resize( world.getRegionCount() );

    for ( Heroes * hero : kingdom.GetHeroes() ) {
        hero->ResetModes( Heroes::SLEEPER );
        hero->setDimensionDoorUsage( 0 );
    }

    const int mapSize = world.w() * world.h();
    for ( int idx = 0; idx < mapSize; ++idx ) {
        const Maps::Tile & tile = world.getTile( idx );
        if ( tile.isFog( myColor ) ) {
            continue;
        }
        const MP2::MapObjectType objectType = tile.getMainObjectType();
        if ( MP2::isInGameActionObject( objectType ) ) {
            _mapActionObjects.try_emplace( idx, objectType );
        }
    }

    evaluateRegionSafety();
    updateKingdomBudget( kingdom );

    Interface::StatusPanel & status = Interface::AdventureMap::Get().getStatusPanel();
    status.drawAITurnProgress( 0 );

    System::MakeDirectory( agentDir() );

    const std::string commandsPath = agentPath( "commands.txt" );
    const std::string statePath = agentPath( "state.json" );
    const std::string logPath = agentPath( "log.txt" );

    uint32_t seq = static_cast<uint32_t>( std::stoul( "0" + trim( readFile( agentPath( "seq" ) ) ) ) );
    std::string lastResults;

    LocalEvent & le = LocalEvent::Get();

    while ( true ) {
        ++seq;
        writeFileAtomic( agentPath( "seq" ), std::to_string( seq ) );
        writeFileAtomic( statePath, agentStateJson( kingdom, lastResults, seq ) );

        // Wait for commands while keeping the window responsive.
        std::string commands;
        while ( true ) {
            if ( !le.HandleEvents( true, true ) ) {
                return fheroes2::GameMode::QUIT_GAME;
            }
            commands = readFile( commandsPath );
            if ( !commands.empty() ) {
                std::remove( commandsPath.c_str() );
                break;
            }
        }

        {
            std::ofstream log( logPath, std::ios::app );
            log << "--- day " << world.GetDay() << " week " << world.GetWeek() << " month " << world.GetMonth() << " seq " << seq << "\n" << commands << "\n";
        }

        std::ostringstream results;
        results << "[";
        bool firstResult = true;
        bool endTurn = false;

        auto report = [&results, &firstResult]( const std::string & command, const bool ok, const std::string & detail ) {
            if ( !firstResult ) {
                results << ",";
            }
            firstResult = false;
            results << "\n  {\"command\":" << esc( command ) << ",\"ok\":" << ( ok ? "true" : "false" ) << ",\"detail\":" << esc( detail ) << "}";
        };

        std::istringstream lines( commands );
        std::string line;
        while ( std::getline( lines, line ) ) {
            line = trim( line );
            if ( line.empty() || line[0] == '#' ) {
                continue;
            }

            std::istringstream args( line );
            std::string cmd;
            args >> cmd;

            try {

            if ( cmd == "end" ) {
                endTurn = true;
                report( line, true, "turn ended" );
                break;
            }

            if ( cmd == "wait" ) {
                report( line, true, "state refreshed" );
                continue;
            }

            if ( cmd == "move" ) {
                int id = -1;
                int x = -1;
                int y = -1;
                args >> id >> x >> y;
                Heroes * hero = findHero( kingdom, id );
                if ( hero == nullptr ) {
                    report( line, false, "no such hero" );
                    continue;
                }
                if ( !Maps::isValidAbsPoint( x, y ) ) {
                    report( line, false, "target outside the map" );
                    continue;
                }
                const int32_t target = Maps::GetIndexFromAbsPoint( x, y );
                _pathfinder.reEvaluateIfNeeded( *hero );
                if ( _pathfinder.getDistance( target ) == 0 ) {
                    // The cached evaluation can be stale after map changes made outside of the pathfinder's knowledge; retry from scratch.
                    _pathfinder.reset();
                    _pathfinder.reEvaluateIfNeeded( *hero );
                }
                if ( _pathfinder.getDistance( target ) == 0 ) {
                    report( line, false,
                            "target is not reachable (hero at " + std::to_string( hero->GetCenter().x ) + "," + std::to_string( hero->GetCenter().y ) + ", target index "
                                + std::to_string( target ) + ", hero index " + std::to_string( hero->GetIndex() ) + ")" );
                    continue;
                }

                const MP2::MapObjectType targetTypeBefore = world.getTile( target ).getMainObjectType();
                const std::string heroName = hero->GetName();
                const std::string armyBefore = hero->GetArmy().String();
                const uint32_t troopsBefore = hero->GetArmy().getTotalCount();

                fheroes2::GameMode gameState = fheroes2::GameMode::END_TURN;
                bool fought = false;

                // The built-in movement stops when new things get revealed, so keep walking (a few times at most) while
                // nothing happened and the target is still there and reachable.
                for ( int attempt = 0; attempt < 6; ++attempt ) {
                    const int prevIndex = hero->GetIndex();
                    const uint32_t troopsNow = hero->GetArmy().getTotalCount();

                    hero->GetPath().setPath( _pathfinder.buildPath( target ) );
                    gameState = HeroesMove( *hero );

                    updateMapActionObjectCache( prevIndex );
                    if ( !hero->isActive() ) {
                        fought = true;
                        break;
                    }
                    updateMapActionObjectCache( hero->GetIndex() );
                    hero->GetPath().Reset();

                    if ( gameState != fheroes2::GameMode::END_TURN ) {
                        break;
                    }
                    if ( hero->GetArmy().getTotalCount() != troopsNow ) {
                        fought = true;
                        break;
                    }
                    if ( hero->GetIndex() == target || hero->GetIndex() == prevIndex || hero->GetMovePoints() == 0 ) {
                        break;
                    }
                    if ( world.getTile( target ).getMainObjectType() != targetTypeBefore ) {
                        // The object was picked up or visited from the adjacent tile.
                        break;
                    }
                    _pathfinder.reEvaluateIfNeeded( *hero );
                    if ( _pathfinder.getDistance( target ) == 0 ) {
                        break;
                    }
                }

                std::ostringstream detail;
                if ( !hero->isActive() ) {
                    detail << "BATTLE LOST: " << heroName << " was defeated (army before: " << armyBefore << ")";
                }
                else {
                    detail << "now at " << hero->GetCenter().x << "," << hero->GetCenter().y;
                    const MP2::MapObjectType targetTypeAfter = world.getTile( target ).getMainObjectType();
                    if ( hero->GetIndex() == target ) {
                        detail << " (arrived)";
                    }
                    else if ( targetTypeAfter != targetTypeBefore ) {
                        detail << " (target " << safe( MP2::StringObject( targetTypeBefore ) ) << " is now " << safe( MP2::StringObject( targetTypeAfter ) ) << ")";
                    }
                    else {
                        detail << " (stopped before the target)";
                    }
                    detail << ", move points left " << hero->GetMovePoints();
                    if ( fought || hero->GetArmy().getTotalCount() < troopsBefore ) {
                        detail << ", BATTLE WON, army " << armyBefore << " -> " << hero->GetArmy().String();
                    }
                    else if ( hero->GetArmy().getTotalCount() != troopsBefore ) {
                        detail << ", army changed: " << armyBefore << " -> " << hero->GetArmy().String();
                    }
                }
                report( line, true, detail.str() );

                if ( gameState != fheroes2::GameMode::END_TURN ) {
                    return gameState;
                }
                continue;
            }

            if ( cmd == "build" ) {
                int x = -1;
                int y = -1;
                uint32_t building = 0;
                args >> x >> y >> building;
                Castle * castle = findCastle( kingdom, x, y );
                if ( castle == nullptr ) {
                    report( line, false, "no castle of mine at that position" );
                    continue;
                }
                const BuildingStatus st = castle->CheckBuyBuilding( building );
                if ( st != BuildingStatus::ALLOW_BUILD ) {
                    report( line, false, buildingStatusName( st ) );
                    continue;
                }
                const bool ok = castle->BuyBuilding( building );
                report( line, ok, ok ? std::string( "built " ) + safe( Castle::GetStringBuilding( building, castle->GetRace() ) ) : "failed" );
                continue;
            }

            if ( cmd == "recruit" ) {
                int x = -1;
                int y = -1;
                uint32_t dwelling = 0;
                std::string countStr;
                args >> x >> y >> dwelling >> countStr;
                Castle * castle = findCastle( kingdom, x, y );
                if ( castle == nullptr ) {
                    report( line, false, "no castle of mine at that position" );
                    continue;
                }
                if ( !castle->isBuild( dwelling ) ) {
                    report( line, false, "dwelling not built" );
                    continue;
                }
                const Monster monster( castle->GetRace(), castle->GetActualDwelling( dwelling ) );
                const uint32_t limit = castle->getRecruitLimit( monster, kingdom.GetFunds() );
                uint32_t count = limit;
                if ( countStr != "max" && !countStr.empty() ) {
                    count = std::min( limit, static_cast<uint32_t>( std::max( 0, std::atoi( countStr.c_str() ) ) ) );
                }
                if ( count == 0 ) {
                    report( line, false, "nothing to recruit (none available or not enough resources)" );
                    continue;
                }
                const bool ok = castle->RecruitMonster( Troop( monster, count ), false );
                report( line, ok, ok ? "recruited " + std::to_string( count ) + " " + safe( monster.GetPluralName( count ) ) : "no free army slot" );
                continue;
            }

            if ( cmd == "upgrade" ) {
                int x = -1;
                int y = -1;
                args >> x >> y;
                Castle * castle = findCastle( kingdom, x, y );
                if ( castle == nullptr ) {
                    report( line, false, "no castle of mine at that position" );
                    continue;
                }
                castle->GetArmy().UpgradeTroops( *castle );
                if ( Heroes * guest = castle->GetHero() ) {
                    guest->GetArmy().UpgradeTroops( *castle );
                }
                report( line, true, "upgraded what was affordable" );
                continue;
            }

            if ( cmd == "hire" ) {
                int x = -1;
                int y = -1;
                int slot = 1;
                args >> x >> y >> slot;
                Castle * castle = findCastle( kingdom, x, y );
                if ( castle == nullptr ) {
                    report( line, false, "no castle of mine at that position" );
                    continue;
                }
                std::string why;
                if ( !castle->AllowBuyHero( &why ) ) {
                    report( line, false, why.empty() ? "cannot hire a hero here now" : why );
                    continue;
                }
                const Recruits & recruits = kingdom.GetRecruits();
                Heroes * candidate = ( slot == 2 ) ? recruits.GetHero2() : recruits.GetHero1();
                if ( candidate == nullptr ) {
                    report( line, false, "no hero in that slot" );
                    continue;
                }
                Heroes * hired = castle->RecruitHero( candidate );
                if ( hired ) {
                    updateMapActionObjectCache( hired->GetIndex() );
                    report( line, true, "hired " + hired->GetName() + " (id " + std::to_string( hired->GetID() ) + ")" );
                }
                else {
                    report( line, false, "hiring failed" );
                }
                continue;
            }

            if ( cmd == "take_garrison" ) {
                int x = -1;
                int y = -1;
                args >> x >> y;
                Castle * castle = findCastle( kingdom, x, y );
                if ( castle == nullptr || castle->GetHero() == nullptr ) {
                    report( line, false, "no castle of mine with a hero inside at that position" );
                    continue;
                }
                Heroes * guest = castle->GetHero();
                guest->GetArmy().JoinStrongestFromArmy( castle->GetArmy() );
                report( line, true, "hero army: " + guest->GetArmy().String() );
                continue;
            }

            if ( cmd == "leave_troop" ) {
                int id = -1;
                size_t slot = 99;
                args >> id >> slot;
                Heroes * hero = findHero( kingdom, id );
                if ( hero == nullptr ) {
                    report( line, false, "no such hero" );
                    continue;
                }
                Castle * castle = hero->inCastleMutable();
                if ( castle == nullptr ) {
                    report( line, false, "hero is not in a castle" );
                    continue;
                }
                Army & army = hero->GetArmy();
                Troop * troop = army.GetTroop( slot );
                if ( troop == nullptr || !troop->isValid() ) {
                    report( line, false, "empty slot" );
                    continue;
                }
                if ( army.getTotalCount() == troop->GetCount() ) {
                    report( line, false, "a hero must keep at least one troop" );
                    continue;
                }
                if ( !castle->GetArmy().JoinTroop( *troop ) ) {
                    report( line, false, "garrison has no free slot" );
                    continue;
                }
                troop->Reset();
                report( line, true, "garrison: " + castle->GetArmy().String() );
                continue;
            }

            report( line, false, "unknown command" );
            }
            catch ( const std::exception & ex ) {
                report( line, false, std::string( "internal error: " ) + ex.what() );
            }
        }

        results << "\n ]";
        lastResults = results.str();

        if ( endTurn ) {
            break;
        }
    }

    // Tell the agent the turn is over.
    std::string finalState = agentStateJson( kingdom, lastResults, ++seq );
    const std::string waiting = "\"status\":\"waiting_for_commands\"";
    if ( const size_t pos = finalState.find( waiting ); pos != std::string::npos ) {
        finalState.replace( pos, waiting.size(), "\"status\":\"opponent_turn\"" );
    }
    writeFileAtomic( agentPath( "seq" ), std::to_string( seq ) );
    writeFileAtomic( statePath, finalState );

    status.resetAITurnProgress();

    return fheroes2::GameMode::END_TURN;
}
