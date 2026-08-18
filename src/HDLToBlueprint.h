//
// Created by georg on 15/07/2026.
//

#pragma once
#include "HDLBaseVisitor.h"
#include "Blueprint.h"
#include "Utils.h"
#include <fstream>
#include <any>

class HDLToBlueprint final : public HDLBaseVisitor {
    Blueprint &bp;
    const std::vector<std::string>& search_paths;

    [[nodiscard]] bool is_constant_or_generic(const std::string& name) const {
        if (bp.constants.contains(name)) return true;
        return std::ranges::find(bp.generic_params, name) != bp.generic_params.end();
    }

    void require_unique_name(const std::string& name) const {
        if (bp.wires.contains(name))
            throw std::runtime_error("Name collision: '" + name + "' is already declared as a wire.");
        if (bp.constants.contains(name))
            throw std::runtime_error("Name collision: '" + name + "' is already declared as a constant.");
        if (std::ranges::find(bp.generic_params, name) != bp.generic_params.end())
            throw std::runtime_error("Name collision: '" + name + "' is already declared as a generic.");
    }

    Bundle evaluate_lhs(HDLParser::LhsContext *ctx) {
        return std::any_cast<Bundle>(visit(ctx));
    }

    Bundle evaluate_rhs(HDLParser::RhsContext *ctx) {
        return std::any_cast<Bundle>(visit(ctx));
    }

    Wire make_wire(const HDLParser::SignalContext *ctx) const {
        const std::string name = ctx->name->getText();

        std::vector<std::string> dims;
        for (HDLParser::ScalarContext *idx_ctx: ctx->indices) {
            const std::string dim = idx_ctx->getText();

            if (!isdigit(dim[0]) && !is_constant_or_generic(dim))
                throw std::runtime_error(std::format("Invalid dimension size '{}' for wire '{}'. Must be an integer, constant or generic.", dim, name));
            dims.push_back(dim);
        }

        return Wire{name, dims};
    }

    Slice make_slice(HDLParser::SliceContext *ctx, const std::string &rep_count = "1") const {
        const std::string wire_name = ctx->signal()->name->getText();

        if (!bp.wires.contains(wire_name) && !bp.constants.contains(wire_name) && wire_name != "_")
            bp.wires[wire_name] = Wire{wire_name, {}};

        std::vector<std::string> dims;
        for (HDLParser::ScalarContext *idx_ctx: ctx->signal()->indices)
            dims.push_back(idx_ctx->getText());

        const std::string lsb = ctx->lsb ? ctx->lsb->getText() : "";
        const std::string msb = ctx->msb ? ctx->msb->getText() : "";

        return Slice{wire_name, dims, lsb, msb, rep_count};
    }

    Chunk evaluate_lhs_item(HDLParser::LhsItemContext *ctx) const {
        if (ctx->UNDERSCORE())
            return Slice{"_", {}, "", "", "1"};

        if (const std::string name = ctx->slice()->signal()->name->getText(); is_constant_or_generic(name))
            throw std::runtime_error("Invalid assignment: Cannot assign data to constant or generic '" + name + "'");

        return make_slice(ctx->slice());
    }

    Chunk evaluate_rhs_item(HDLParser::RhsItemContext *ctx) const {
        if (ctx->LITERAL())
            return Literal{ctx->LITERAL()->getText(), "1"};

        if (ctx->slice()) {
            const std::string name = ctx->slice()->signal()->name->getText();

            if (bp.constants.contains(name))
                return Literal{bp.constants.at(name), "1"};
            if (std::ranges::find(bp.generic_params, name) != bp.generic_params.end())
                return Literal{name, "1"};

            return make_slice(ctx->slice());
        }

        // Must be replication rule
        const auto rep = ctx->replication();
        const std::string count = rep->count->getText();

        if (rep->val->LITERAL())
            return Literal{rep->val->LITERAL()->getText(), count};

        const std::string name = rep->val->slice()->signal()->name->getText();

        if (bp.constants.contains(name))
            return Literal{bp.constants.at(name), count};
        if (std::ranges::find(bp.generic_params, name) != bp.generic_params.end())
            return Literal{name, count};

        return make_slice(rep->val->slice(), count);
    }

public:
    explicit HDLToBlueprint(Blueprint &bp, const std::vector<std::string>& paths)
        : bp(bp), search_paths(paths) {}

    antlrcpp::Any visitChip(HDLParser::ChipContext *ctx) override {
        bp.name = ctx->name->getText();
        return visitChildren(ctx);
    }

    antlrcpp::Any visitFlag(HDLParser::FlagContext *ctx) override {
        std::optional<std::string> data = std::nullopt;
        if (ctx->data) data = ctx->data->getText();
        const std::string flag_name = ctx->name->getText();

        bp.flags[flag_name].push_back(data);

        // Load constants
        if (flag_name == "const" && data.has_value()) {
            const std::string const_filepath = Utils::resolve_file(data.value() + ".const", search_paths);
            std::ifstream stream(const_filepath);
            if (!stream.is_open()) throw std::runtime_error("Could not open const file: " + const_filepath);

            std::string line;
            while (std::getline(stream, line)) {
                if (const size_t pos = line.find("//"); pos != std::string::npos) line = line.substr(0, pos);
                line.erase(0, line.find_first_not_of(" \t\r\n")); if (line.empty()) continue;
                line.erase(line.find_last_not_of(" \t\r\n") + 1); if (line.empty()) continue;

                const size_t eq_pos = line.find('=');
                if (eq_pos == std::string::npos) throw std::runtime_error("Invalid const definition: " + line);

                std::string id = line.substr(0, eq_pos); std::string val = line.substr(eq_pos + 1);
                id.erase(0, id.find_first_not_of(" \t\r\n")); id.erase(id.find_last_not_of(" \t\r\n") + 1);
                val.erase(0, val.find_first_not_of(" \t\r\n")); val.erase(val.find_last_not_of(" \t\r\n") + 1);
                if (!val.empty() && val.back() == ';') { val.pop_back(); val.erase(val.find_last_not_of(" \t\r\n") + 1); }

                require_unique_name(id);
                bp.constants[id] = val;
            }
        }
        return nullptr;
    }

    antlrcpp::Any visitGenericParams(HDLParser::GenericParamsContext *ctx) override {
        for (const antlr4::Token *param : ctx->params) {
            const std::string name = param->getText();
            require_unique_name(name);
            bp.generic_params.push_back(name);
        }
        return nullptr;
    }

    antlrcpp::Any visitConstSection(HDLParser::ConstSectionContext *ctx) override {
        for (HDLParser::ConstDefContext *const_ctx : ctx->constants) {
            const std::string name = const_ctx->ID()->getText();
            require_unique_name(name);
            bp.constants[name] = const_ctx->number()->getText();
        }
        return nullptr;
    }

    antlrcpp::Any visitInSection(HDLParser::InSectionContext *ctx) override {
        for (const HDLParser::SignalContext *sig_ctx: ctx->sigs) {
            Wire w = make_wire(sig_ctx);
            bp.wires[w.name] = w;
            bp.in_wires.push_back(w.name);
        }
        return nullptr;
    }

    antlrcpp::Any visitOutSection(HDLParser::OutSectionContext *ctx) override {
        for (const HDLParser::SignalContext *sig_ctx: ctx->sigs) {
            Wire w = make_wire(sig_ctx);
            bp.wires[w.name] = w;
            bp.out_wires.push_back(w.name);
        }
        return nullptr;
    }


    // PARTS

    antlrcpp::Any visitPartInst(HDLParser::PartInstContext *ctx) override {
        std::vector<std::string> generic_args{};
        if (ctx->genericArgs()) {
            for (HDLParser::NumberContext *num_ctx: ctx->genericArgs()->args)
                generic_args.push_back(num_ctx->getText());
        }

        std::unordered_map<std::string, Bundle> pins{};
        if (ctx->partArgs()) {
            for (const HDLParser::ConnectionContext *conn: ctx->partArgs()->conns) {
                const std::string pin_name = conn->param->getText();
                pins.emplace(pin_name, evaluate_rhs(conn->val));
            }
        }

        std::string tag;
        if (ctx->partTag()) tag = ctx->partTag()->tag->getText();

        const std::string type = ctx->type->getText();

        bp.parts.push_back(Part{type, generic_args, pins, tag});
        return nullptr;
    }

    antlrcpp::Any visitPartAssign(HDLParser::PartAssignContext *ctx) override {
        const Bundle left_bundle = evaluate_lhs(ctx->lhs());
        const Bundle right_bundle = evaluate_rhs(ctx->rhs());

        const std::unordered_map<std::string, Bundle> pins = {
            {"out", left_bundle},
            {"in", right_bundle}
        };

        bp.parts.push_back(Part{"__Assign", {}, pins});
        return nullptr;
    }

    // LHS / RHS

    antlrcpp::Any visitLhsSingle(HDLParser::LhsSingleContext *ctx) override {
        return Bundle{evaluate_lhs_item(ctx->lhsItem())};
    }

    antlrcpp::Any visitLhsConcat(HDLParser::LhsConcatContext *ctx) override {
        Bundle combined_bundle;
        for (HDLParser::LhsItemContext *item: ctx->items)
            combined_bundle.push_back(evaluate_lhs_item(item));
        return combined_bundle;
    }

    antlrcpp::Any visitRhsSingle(HDLParser::RhsSingleContext *ctx) override {
        return Bundle{evaluate_rhs_item(ctx->rhsItem())};
    }

    antlrcpp::Any visitRhsConcat(HDLParser::RhsConcatContext *ctx) override {
        Bundle combined_bundle;
        for (HDLParser::RhsItemContext *item: ctx->items)
            combined_bundle.push_back(evaluate_rhs_item(item));
        return combined_bundle;
    }

    antlrcpp::Any visitRhsDiscard(HDLParser::RhsDiscardContext *ctx) override {
        return Bundle{};
    }
};
