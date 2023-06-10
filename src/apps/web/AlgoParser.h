#include <iostream>
#include <string>

#include <tao/pegtl.hpp>
#include <re2/re2.h>

#include "events.h"
#include "Bech32Utils.h"




struct AlgoCompiled {
    double threshold = 20;
    using PubkeySet = flat_hash_set<std::string>;
    std::vector<PubkeySet> pubkeySets;
    flat_hash_map<std::string, uint64_t> variableIndexLookup; // variableName -> index into pubkeySets

    PubkeySet *mods = nullptr;
    PubkeySet *voters = nullptr;

    struct Filter {
        std::unique_ptr<RE2> re;
        char op;
        double arg;
    };

    std::vector<Filter> filters;

    void updateScore(lmdb::txn &txn, Decompressor &decomp, const defaultDb::environment::View_Event &e, double &score) {
        auto rawJson = getEventJson(txn, decomp, e.primaryKeyId);
        re2::StringPiece rawJsonSP(rawJson);

        for (const auto &f : filters) {
            if (!RE2::PartialMatch(rawJsonSP, *f.re)) continue;

            if (f.op == '+') score += f.arg;
            else if (f.op == '-') score -= f.arg;
            else if (f.op == '*') score *= f.arg;
            else if (f.op == '/') score /= f.arg;
        }
    }
};


struct AlgoParseState {
    lmdb::txn &txn;

    AlgoCompiled a;

    struct ExpressionState {
        std::string currInfixOp;
        AlgoCompiled::PubkeySet set;
    };

    std::vector<ExpressionState> expressionStateStack;
    std::string currPubkeyDesc;
    std::vector<std::string> currModifiers;

    std::string currSetterVar;
    char currFilterOp;
    double currFilterArg;

    AlgoParseState(lmdb::txn &txn) : txn(txn) {}

    void letStart(std::string_view name) {
        if (a.variableIndexLookup.contains(name)) throw herr("overwriting variable: ", name);
        a.variableIndexLookup[name] = a.pubkeySets.size();
        expressionStateStack.push_back({ "+" });
    }

    void letEnd() {
        a.pubkeySets.emplace_back(std::move(expressionStateStack.back().set));
        expressionStateStack.clear();
    }

    void letAddExpression() {
        const auto &id = currPubkeyDesc;
        AlgoCompiled::PubkeySet set;

        if (id.starts_with("npub1")) {
            set.insert(decodeBech32Simple(id));
        } else {
            if (!a.variableIndexLookup.contains(id)) throw herr("variable not found: ", id);
            auto n = a.variableIndexLookup[id];
            if (n >= a.pubkeySets.size()) throw herr("self referential variable: ", id);
            set = a.pubkeySets[n];
        }

        for (const auto &m : currModifiers) {
            if (m == "following") {
                AlgoCompiled::PubkeySet newSet = set;
                for (const auto &p : set) loadFollowing(p, newSet);
                set = newSet;
            } else {
                throw herr("unrecognised modifier: ", m);
            }
        }

        currPubkeyDesc = "";
        currModifiers.clear();

        mergeInfix(set);
    }

    void mergeInfix(AlgoCompiled::PubkeySet &set) {
        auto &currInfixOp = expressionStateStack.back().currInfixOp;

        if (currInfixOp == "+") {
            for (const auto &e : set) {
                expressionStateStack.back().set.insert(e);
            }
        } else if (currInfixOp == "-") {
            for (const auto &e : set) {
                expressionStateStack.back().set.erase(e);
            }
        } else if (currInfixOp == "&") {
            AlgoCompiled::PubkeySet intersection;

            for (const auto &e : set) {
                if (expressionStateStack.back().set.contains(e)) intersection.insert(e);
            }

            std::swap(intersection, expressionStateStack.back().set);
        }
    }



    void installSetter(std::string_view val) {
        if (currSetterVar == "mods" || currSetterVar == "voters") {
            if (!a.variableIndexLookup.contains(val)) throw herr("unknown variable: ", val);
            auto *setPtr = &a.pubkeySets[a.variableIndexLookup[val]];

            if (currSetterVar == "mods") a.mods = setPtr;
            else if (currSetterVar == "voters") a.voters = setPtr;
        } else if (currSetterVar == "threshold") {
            a.threshold = std::stod(std::string(val));
        }
    }

    void installFilter(std::string_view val) {
        a.filters.emplace_back(std::make_unique<RE2>(val), currFilterOp, currFilterArg);
    }





    void loadFollowing(std::string_view pubkey, flat_hash_set<std::string> &output) {
        const uint64_t kind = 3;

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, kind, 0), "", [&](std::string_view k, std
::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == kind) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                auto ev = lookupEventByLevId(txn, levId);

                for (const auto &tagPair : *(ev.flat_nested()->tagsFixed32())) {
                    if ((char)tagPair->key() != 'p') continue;
                    output.insert(std::string(sv(tagPair->val())));
                }
            }

            return false;
        });
    }
};




namespace pegtl = TAO_PEGTL_NAMESPACE;

namespace algo_parser {
    // Whitespace

    struct comment :
        pegtl::seq<
            pegtl::one< '#' >,
            pegtl::until< pegtl::eolf >
        > {};

    struct ws : pegtl::sor< pegtl::space, comment > {};

    template< typename R >
    struct pad : pegtl::pad< R, ws > {};


    // Pubkeys

    struct npub :
        pegtl::seq<
            pegtl::string< 'n', 'p', 'u', 'b', '1' >,
            pegtl::plus< pegtl::alnum >
        > {};

    struct pubkey :
        pegtl::sor<
            npub,
            pegtl::identifier
        > {};

    struct pubkeySetOp : pegtl::one< '+', '-', '&' > {};

    struct pubkeyGroup;
    struct pubkeyList : pegtl::list< pubkeyGroup, pubkeySetOp, ws > {};

    struct pubkeyGroupOpen : pegtl::one< '(' > {};
    struct pubkeyGroupClose : pegtl::one< ')' > {};

    struct pubkeyModifier : pegtl::identifier {};
    struct pubkeyExpression : pegtl::seq<
        pubkey,
        pegtl::star< pegtl::seq< pegtl::one< '.'>, pubkeyModifier > >
    > {};

    struct pubkeyGroup : pegtl::sor<
        pubkeyExpression,
        pegtl::seq<
            pad< pubkeyGroupOpen >,
            pubkeyList,
            pad< pubkeyGroupClose >
        >
    > {};



    // Let statements

    struct variableIdentifier : pegtl::seq< pegtl::not_at< npub >, pegtl::identifier > {};

    struct letDefinition : variableIdentifier {};
    struct letTerminator : pegtl::one< ';' > {};

    struct let :
        pegtl::seq<
            pad< TAO_PEGTL_STRING("let") >,
            pad< letDefinition >,
            pad< pegtl::one< '=' > >,
            pad< pubkeyList >,
            letTerminator
        > {};




    // Posts block

    struct number :
        pegtl::if_then_else< pegtl::one< '.' >,
            pegtl::plus< pegtl::digit >,
            pegtl::seq<
                pegtl::plus< pegtl::digit >,
                pegtl::opt< pegtl::one< '.' >, pegtl::star< pegtl::digit > >
            >
        > {};

    struct arithOp : pegtl::one< '+', '-', '*', '/' > {};
    struct arithNumber : number {};
    struct arith :
        pegtl::seq<
            pad< arithOp >,
            arithNumber
        > {};

    struct regexpPayload : pegtl::star< pegtl::sor< pegtl::string< '\\', '/' >, pegtl::not_one< '/' > > > {};
    struct regexp :
        pegtl::seq<
            pegtl::one< '/' >,
            regexpPayload,
            pegtl::one< '/' >
        > {};

    struct contentCondition :
        pegtl::seq<
            pad< pegtl::one< '~' > >,
            pad< regexp >
        > {};

    struct condition :
        pegtl::sor<
            pad< contentCondition >
        > {};

    struct setterVar :
        pegtl::sor<
            TAO_PEGTL_STRING("mods"),
            TAO_PEGTL_STRING("voters"),
            TAO_PEGTL_STRING("threshold")
        > {};

    struct setterValue :
        pegtl::star<
            pegtl::sor<
                pegtl::alnum,
                pegtl::one< '.' >
            >
        > {};

    struct setterStatement :
        pegtl::seq<
            pad< setterVar >,
            pad< TAO_PEGTL_STRING("=") >,
            pad< setterValue >,
            pegtl::one< ';' >
        > {};

    struct filterStatment :
        pegtl::seq<
            pad< arith >,
            pad< TAO_PEGTL_STRING("if") >,
            pad< condition >,
            pegtl::one< ';' >
        > {};

    struct postBlock :
        pegtl::seq<
            pad< TAO_PEGTL_STRING("posts") >,
            pad< pegtl::one< '{' > >,
            pegtl::star< pad< pegtl::sor< setterStatement, filterStatment > > >,
            pegtl::one< '}' >
        > {};




    // Main

    struct anything : pegtl::sor< ws, let, postBlock > {};
    struct main : pegtl::until< pegtl::eof, pegtl::must< anything > > {};



    template< typename Rule >
    struct action {};


    template<> struct action< letDefinition > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.letStart(in.string_view());
        }
    };

    template<> struct action< letTerminator > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.letEnd();
        }
    };

    template<> struct action< pubkey > { template< typename ActionInput >
        static void apply(const ActionInput& in, AlgoParseState &a) {
            a.currPubkeyDesc = in.string();
        }
    };

    template<> struct action< pubkeyModifier > { template< typename ActionInput >
        static void apply(const ActionInput& in, AlgoParseState &a) {
            a.currModifiers.push_back(in.string());
        }
    };

    template<> struct action< pubkeySetOp > { template< typename ActionInput >
        static void apply(const ActionInput& in, AlgoParseState &a) {
            a.expressionStateStack.back().currInfixOp = in.string();
        }
    };

    template<> struct action< pubkeyExpression > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.letAddExpression();
        }
    };

    template<> struct action< pubkeyGroupOpen > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.expressionStateStack.push_back({ "+" });
        }
    };

    template<> struct action< pubkeyGroupClose > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            auto set = std::move(a.expressionStateStack.back().set);
            a.expressionStateStack.pop_back();
            a.mergeInfix(set);
        }
    };



    template<> struct action< setterVar > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.currSetterVar = in.string();
        }
    };

    template<> struct action< setterValue > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.installSetter(in.string_view());
        }
    };

    template<> struct action< arithOp > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.currFilterOp = in.string_view().at(0);
        }
    };

    template<> struct action< arithNumber > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.currFilterArg = std::stod(in.string());
        }
    };

    template<> struct action< regexpPayload > { template< typename ActionInput >
        static void apply(const ActionInput &in, AlgoParseState &a) {
            a.installFilter(in.string_view());
        }
    };
}


inline AlgoCompiled parseAlgo(lmdb::txn &txn, std::string_view algoText) {
    AlgoParseState a(txn);

    pegtl::memory_input in(algoText, "");

    if (!pegtl::parse< algo_parser::main, algo_parser::action >(in, a)) {
        throw herr("algo parse error");
    }

    return std::move(a.a);
}
