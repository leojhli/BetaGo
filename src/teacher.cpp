#include "betago/teacher.hpp"
#include <chrono>
#include <cmath>
#include <limits>

namespace betago {
namespace {
using Clock = std::chrono::steady_clock;

double elapsed_since(Clock::time_point began) {
    return std::chrono::duration<double>(Clock::now() - began).count();
}

Json move_json(const Move& move) {
    return move ? Json::array({move->row, move->column}) : Json(nullptr);
}

Json unexpected_failure(const std::exception& error, const std::string& phase) {
    return {{"code", "teacher_error"}, {"command", phase}, {"diagnostics", error.what()}};
}
} // namespace

void TeacherSettings::validate() const {
    if (board_size < 1 || board_size > 19)
        throw std::invalid_argument("Teacher board size must be 1..19 for expert SGF records");
    if (!std::isfinite(komi)) throw std::invalid_argument("Teacher komi must be finite");
    if (games < 1) throw std::invalid_argument("Teacher game count must be positive");
    if (max_moves < 1 || max_moves > 200000)
        throw std::invalid_argument("Teacher move limit must be 1..200000 for expert records");
    if (seed > std::numeric_limits<std::int64_t>::max() - (static_cast<std::int64_t>(games) - 1))
        throw std::invalid_argument("Teacher seeds exceed signed 64-bit range");
}

Json TeacherSettings::to_json() const {
    return {{"board_size", board_size}, {"komi", komi}, {"games", games},
            {"max_moves", max_moves}, {"seed", seed}};
}

Json generate_teacher_games(const ExternalGtpConfiguration& configuration,
                            const TeacherSettings& settings,
                            const TeacherProgress& progress,
                            const TeacherSnapshot& snapshot) {
    configuration.validate();
    settings.validate();
    ExternalGtpConfiguration prepared = configuration;
    const Json initial_identity = external_gtp_identity(configuration);
    if (prepared.declared_file_fingerprints.is_null())
        prepared.declared_file_fingerprints = initial_identity;
    const auto began = Clock::now();
    Json corpus = {{"schema_version", 1}, {"kind", "expert_game_corpus"},
        {"settings", settings.to_json()}, {"games", Json::array()}, {"attempts", Json::array()},
        {"summary", {{"attempted", 0}, {"completed", 0}, {"failures", 0},
                     {"resignations", 0}, {"truncations", 0}}},
        {"metadata", {{"provenance", "teacher_self_play"}, {"professional_game_data", false},
            {"rank_calibration", false}, {"teacher_configuration", prepared.to_json()},
            {"external_identity", initial_identity},
            {"rules_verification", configuration.adapter == "katago" ? "see_session_metadata" : "unverified_generic"},
            {"rules", "BetaGo simple ko, no suicide, raw area scoring, no dead-group adjudication"},
            {"seed_derivation", "base seed + zero-based game index"},
            {"diversity_note", "Distinct configured seeds do not guarantee distinct games; pretraining deduplicates canonical games."}}},
        {"elapsed_seconds", 0.0}};

    for (int index = 0; index < settings.games; ++index) {
        const auto game_began = Clock::now();
        const std::int64_t seed = settings.seed + static_cast<std::int64_t>(index);
        const GameState initial = GameState::new_game(settings.board_size, settings.komi);
        GameState state = initial;
        std::vector<Move> moves;
        Json decisions = Json::array();
        Json failure = nullptr;
        std::string status = "truncated", reason = "move_limit", phase = "startup";
        ExternalGtpSession session(prepared, seed, settings.board_size, settings.komi);

        // Observer work is outside engine-error catch blocks. An I/O or UI
        // observer failure must not silently become a rejected teacher game.
        if (progress) progress(index, state, PASS, 0);
        bool ready = false;
        try { session.start(); ready = true; }
        catch (const GtpFailure& error) {
            status = "failed"; reason = "engine_failure"; failure = error.to_json();
        } catch (const std::exception& error) {
            status = "failed"; reason = "engine_failure"; failure = unexpected_failure(error, phase);
        }

        for (int ply = 0; ready && ply < settings.max_moves && !state.is_terminal(); ++ply) {
            const int player = state.to_play();
            const auto decision_began = Clock::now();
            Json decision = {{"move_number", ply + 1}, {"player", player}, {"accepted", false}};
            Move move;
            bool accepted = false;
            phase = "genmove";
            try {
                move = session.genmove(state);
                // genmove already checks local legality; applying the successor
                // here keeps acceptance and recorded history authoritative.
                GameState next = state.play(move);
                state = std::move(next);
                moves.push_back(move);
                accepted = true;
                decision["move"] = move_json(move);
                decision["accepted"] = true;
                phase = "accepted_move";
                // genmove changed the external board. Acknowledge its own move
                // without sending GTP play, for Black AND White alike.
                session.accepted_move(player, move, true);
            } catch (const GtpResignation& error) {
                status = "resigned"; reason = "resignation"; ready = false;
                decision["response"] = "resign";
                decision["command"] = error.command;
            } catch (const GtpFailure& error) {
                status = "failed"; reason = "engine_failure"; ready = false;
                failure = error.to_json();
                decision["failure"] = failure;
            } catch (const std::exception& error) {
                status = "failed"; reason = "engine_failure"; ready = false;
                failure = unexpected_failure(error, phase);
                decision["failure"] = failure;
            }
            decision["phase"] = phase;
            decision["elapsed_seconds"] = elapsed_since(decision_began);
            decisions.push_back(std::move(decision));
            if (accepted && progress) progress(index, state, move, static_cast<int>(moves.size()));
        }

        if (ready && state.is_terminal()) { status = "completed"; reason = "two_passes"; }
        Json cleanup = session.shutdown();
        Json session_metadata = session.metadata();
        Json attempt = GameResult{state, moves, settings.max_moves, elapsed_since(game_began)}.to_json();
        attempt["game_index"] = index; attempt["seed"] = seed;
        attempt["status"] = status; attempt["termination_reason"] = reason;
        attempt["accepted"] = status == "completed";
        attempt["failure"] = std::move(failure);
        attempt["decisions"] = std::move(decisions);
        attempt["cleanup_warnings"] = std::move(cleanup);
        attempt["teacher_session"] = session_metadata;
        attempt["game_id"] = nullptr;

        if (status == "completed") {
            ExpertGame game{initial, moves, state.winner().value_or(EMPTY),
                "teacher:" + configuration.display_name,
                {{"provenance", "teacher_self_play"}, {"professional_game_data", false},
                 {"rank_calibration", false}, {"game_index", index}, {"teacher_seed", seed},
                 {"teacher_session", std::move(session_metadata)}, {"score", attempt.at("score")}}};
            attempt["game_id"] = expert_game_id(game);
            corpus["games"].push_back(expert_game_json(game));
        } else {
            // A resignation or cutoff is not a local two-pass result. Keep its
            // legal prefix but provide no value label or invented final score.
            attempt["score"] = nullptr; attempt["winner"] = nullptr;
        }
        auto& summary = corpus["summary"];
        summary["attempted"] = index + 1;
        const char* counter = status == "completed" ? "completed" : status == "failed" ? "failures" :
                              status == "resigned" ? "resignations" : "truncations";
        summary[counter] = summary.at(counter).get<int>() + 1;
        corpus["attempts"].push_back(std::move(attempt));
        corpus["elapsed_seconds"] = elapsed_since(began);
        if (snapshot) snapshot(corpus);
    }
    return corpus;
}
} // namespace betago
