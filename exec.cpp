#include "exec.h"
#include "runtime.h"
#include "docwordspace.h"

using namespace Trinity;

// tightly packed node (4 bytes)
// It's possible that we can use 12 bits for the operand(nodeCtxIdx) and the remaining 4 for the opcode
// and make sizeof(exec_node) == 2
// Eventually we will be able to compile this down to bytecode ro 
// We could also include a weight here -- so that if e.g a phrase or a complex expression matched we'd boost the score
// by some factor/coefficient(this would have been provided by the input query)
struct exec_node
{
	// dereference implementation in an array of void(*)(void)
	// so that we won't have to use 8 bytes for the pointer
	uint8_t implIdx;

	uint8_t flags;

	// instead of using unions here
	// we will have a node/impl specific context allocated elsehwere with the appropriate size
	// so that the implementaiton can refer to it
	uint16_t nodeCtxIdx;
};

static_assert(sizeof(exec_node) == sizeof(uint32_t), "Unexpected sizeof(exec_node)");

// This is initialized by the compiler
// and used by the VM
struct runtime_ctx
{
	Trinity::segment *const seg;

	// See compile()
	struct binop_ctx
	{
		exec_node lhs;
		exec_node rhs;
	};

	// See compile()
	struct unaryop_ctx
	{
		exec_node expr;
	};

	// See compile() and OpCodes::MatchToken
	struct token
	{
		uint8_t rep; 		// see phrase::rep
		uint8_t index; 		// see phrase::index
		exec_term_id_t termID;
	};

	// See compile() and OpCodes::MatchPhrase
	struct phrase
	{
		exec_term_id_t *termIDs;
		uint8_t rep; 		// phrase::rep
		uint16_t index; 	// phrase::index
		uint8_t size;
	};

	// Materialized hits for a term and the current document
	// this is used both for evaluation and for scoring documents
	struct term_hits
	{
		term_hit *all{0};
		uint16_t freq; 
		uint16_t allCapacity{0};
		uint32_t docID;

		void set_freq(const uint16_t newFreq)
                {
                        if (newFreq > allCapacity)
                        {
                                allCapacity = newFreq + 32;
                                if (all)
                                        std::free(all);
                                all = (term_hit *)malloc(sizeof(term_hit) * allCapacity);
                        }

			freq = newFreq;
                }

		~term_hits()
		{
			if (all)
				free(all);
		}
	};



	auto materialize_term_hits(const exec_term_id_t termID)
	{
		auto th = decode_ctx.termHits[termID];

		require(th);

		if (th->docID != curDocID)
		{
			// Not already materialized
			auto dec = decode_ctx.decoders[termID];
			const auto docHits = dec->cur_doc_freq();

			th->docID = curDocID;
			th->set_freq(docHits);
			dec->materialize_hits(termID, &docWordsSpace, th->all);
		}

		return th;
	}

#if 0
	bool materialize_term_hits_with_phrase_prevterm_check(const exec_term_id_t termID, const exec_term_id_t phrasePrevTermID, const uint16_t phraseIdx /* second phrase index=1, etc */)
        {
                auto th = decode_ctx.termHits[termID];

                require(th);

                if (th->docID != curDocID)
                {
                        auto dec = decode_ctx.decoders[termID];
                        const auto docHits = dec->cur_doc_freq();

                        th->docID = curDocID;
                        th->set_freq(docHits);
                        return dec->materialize_hits_with_phrase_prevterm_check(&docWordsSpace, th->all, phrasePrevTermID);
                }
                else
                {
                        const auto freq = th->freq;
                        const auto all = th->all;

                        for (uint32_t i{0}; i != freq; ++i)
                        {
                                if (const auto pos = all[i].pos; pos > phraseIdx)
                                {
                                        if (docWordsSpace.test(phrasePrevTermID, pos - phraseIdx))
                                                return true;
                                }
                        }

                        return false;
                }
        }
#endif

        // This is from the lead tokens
	// We expect all token and phrases opcodes to check against this document
	uint32_t curDocID;




	// For each matched document, the score function will get
	// (documentID, runtime_ctx *, and a list of matched_query_term)
	// You can get the actual query from matched_query_term.hits->termID and
	// runtime_ctx methods that accept that ID
	struct matched_query_term
	{
		uint8_t rep; 	// see token
		uint8_t index; 	// see token
		exec_term_id_t termID;
		term_hits *hits;// we have a term_hits for each distinct query term (e.g decodersTermHits[token.decoderIdx])
	};



	struct
	{
		binop_ctx *binaryOps;
		unaryop_ctx *unaryOps;
		token *tokens;
		phrase *phrases;
	} evalnode_contexts;

        struct
        {
                Trinity::Codecs::Decoder **decoders{nullptr};
                term_hits **termHits{nullptr};
                uint16_t capacity{0};

                void check(const uint16_t idx)
                {
                        if (idx >= capacity)
                        {
                                const auto newCapacity{idx + 8};

                                decoders = (Trinity::Codecs::Decoder **)realloc(decoders, sizeof(Trinity::Codecs::Decoder *) * newCapacity);
                                memset(decoders + capacity, 0, (newCapacity - capacity) * sizeof(Trinity::Codecs::Decoder *));

                                termHits = (term_hits **)realloc(termHits, sizeof(term_hits *) * newCapacity);
                                memset(termHits + capacity, 0, (newCapacity - capacity) * sizeof(term_hits *));

                                capacity = newCapacity;
                        }
                }
        } decode_ctx;

        void setup_evalnode_contexts()
	{
		evalnode_contexts.binaryOps = binOpContexts.data();
		evalnode_contexts.unaryOps = unaryOpContexts.data();
		evalnode_contexts.tokens = registeredTokens.data();
		evalnode_contexts.phrases = registeredPhrases.data();
	};

	// for simplicity's sake, we are just going to map exec_term_id_t => decoders[] without
	// indirection. For each distict/resolved term, we have a decoder and term_hits in decode_ctx.decoders[] and decode_ctx.termHits[]
	// This means you can index them using a termID
	// This means we may have some nullptr in decode_ctx.decoders[] but that's OK
	void prepare_decoder(exec_term_id_t termID)
	{
		decode_ctx.check(termID);

		if (!decode_ctx.decoders[termID])
		{
			decode_ctx.decoders[termID] = seg->new_postings_decoder(term_ctx(termID));
			decode_ctx.termHits[termID] = new term_hits();
		}

		SLog("Initialized decoder for ", termID, "\n");
		require(decode_ctx.decoders[termID]);
	}

	uint16_t register_binop(const exec_node lhs, const exec_node rhs)
	{
		binOpContexts.push_back({lhs, rhs});
		return binOpContexts.size() - 1;
	}

	uint16_t register_unaryop(const exec_node expr)
	{
		unaryOpContexts.push_back({expr});
		return unaryOpContexts.size() - 1;
	}

	uint16_t register_token(const Trinity::phrase *p)
	{
		auto t = registeredTokens.PushEmpty();

		t->rep = p->rep;
		t->index = p->index;
		t->termID = resolve_term(p->terms[0].token);

		prepare_decoder(t->termID);
		return registeredTokens.size() - 1;
	}

	uint16_t register_phrase(const Trinity::phrase *p)
	{
		auto ptr = registeredPhrases.PushEmpty();

		ptr->rep= p->rep;
		ptr->index = p->index;
		ptr->size = p->size;
		ptr->termIDs = (exec_term_id_t *)allocator.Alloc(sizeof(exec_term_id_t) * p->size);

		for (uint32_t i{0}; i != p->size; ++i)
		{
			const auto id = resolve_term(p->terms[i].token);

			prepare_decoder(id);
			ptr->termIDs[i] = id;
		}

		return registeredPhrases.size() - 1;
	}

	uint32_t token_eval_cost(const strwlen8_t token)
	{
		const auto termID = resolve_term(token);

		if (termID == 0)
			return UINT32_MAX;
		
		const auto ctx = term_ctx(termID);

		if (ctx.documents == 0)
			return UINT32_MAX;

		return ctx.documents;
	}

	uint32_t phrase_eval_cost(const Trinity::phrase *const p)
	{
		uint32_t sum{0};

		for (uint32_t i{0}; i != p->size; ++i)
		{
			const auto token = p->terms[i].token;

			if (const auto cost = token_eval_cost(token); cost == UINT32_MAX)
				return UINT32_MAX;
			else
				sum+=cost;
		}

		// Not sure this is the right way to go about it
		return sum;
	}

	runtime_ctx(segment *s)
		: seg{s}, docWordsSpace{4096}
	{

	}

	~runtime_ctx()
	{
		for (uint32_t i{0}; i != decode_ctx.capacity; ++i)
		{
			delete decode_ctx.decoders[i];
			delete decode_ctx.termHits[i];
		}

		if (auto ptr = decode_ctx.decoders)
			std::free(ptr);

		if (auto ptr = decode_ctx.termHits)
			std::free(ptr);
	}

	void reset(const uint32_t did)
	{
		curDocID = did;
		docWordsSpace.reset(did);
	}
		
	
	// used during compilation
	exec_term_id_t resolve_term(const strwlen8_t term)
	{
		exec_term_id_t *ptr;

		if (termsDict.Add(term, 0, &ptr))
		{
			// translate from segment space to runtime_ctx space
			*ptr = termsDict.size();
			toSegmentSpace.insert({*ptr, seg->resolve_term(term)});
		}

		return *ptr;
	}

	// used during compilation
	term_segment_ctx term_ctx(const exec_term_id_t termID)
	{
		return seg->term_ctx(toSegmentSpace[termID]);
	}


	DocWordsSpace docWordsSpace;
	Switch::vector<token> registeredTokens;
	Switch::vector<phrase> registeredPhrases;
	std::vector<binop_ctx> binOpContexts;
	std::vector<unaryop_ctx> unaryOpContexts;
	// decoders for all distinct tokens of the query
	Switch::unordered_map<strwlen8_t, exec_term_id_t> termsDict;
	Switch::unordered_map<exec_term_id_t, uint32_t> toSegmentSpace; 	// translation between runtime_ctx and segment term IDs spaces


	simple_allocator allocator;
};





#pragma mark OPTIMIZER
static uint32_t optimize_binops_impl(ast_node *const n, bool &updates, runtime_ctx &rctx)
{
        switch (n->type)
        {
                case ast_node::Type::Token:
                        if (const auto cost = rctx.token_eval_cost(n->p->terms[0].token); cost == UINT32_MAX)
                        {
                                updates = true;
                                n->set_const_false();
                                return UINT32_MAX;
                        }
                        else
                                return cost;

                case ast_node::Type::Phrase:
                        if (const auto cost = rctx.phrase_eval_cost(n->p); cost == UINT32_MAX)
                        {
                                updates = true;
                                n->set_const_false();
                                return UINT32_MAX;
                        }
                        else
                                return cost;

                case ast_node::Type::BinOp:
                {
                        const auto lhsCost = optimize_binops_impl(n->binop.lhs, updates, rctx);

                        if (lhsCost == UINT32_MAX)
                        {
                                if (n->binop.op == Operator::AND || n->binop.op == Operator::STRICT_AND)
                                {
                                        n->set_const_false();
                                        updates = true;
                                        return UINT32_MAX;
                                }
                        }

                        const auto rhsCost = optimize_binops_impl(n->binop.rhs, updates, rctx);

                        if (rhsCost == UINT32_MAX && lhsCost == UINT32_MAX && n->binop.op == Operator::OR)
                        {
                                n->set_const_false();
                                updates = true;
                                return UINT32_MAX;
                        }

                        if (rhsCost < lhsCost && n->binop.op != Operator::NOT) // can't reorder NOT
                        {
                                std::swap(n->binop.lhs, n->binop.rhs);
                        }

                        return lhsCost + rhsCost;
                }

                case ast_node::Type::UnaryOp:
                        if (const auto cost = optimize_binops_impl(n->unaryop.expr, updates, rctx); cost == UINT32_MAX)
                        {
                                n->set_const_false();
                                updates = true;
                                return UINT32_MAX;
                        }
                        else
                                return cost;

                case ast_node::Type::ConstFalse:
                        return UINT32_MAX;

                default:
                        break;
        }

        return 0;
}

// similar to reorder_root(), except this time, binary ops take into account the cost to evaluate each branch
// and potentially swaps LHS and RHS for binary ops, or even sets nodes to ConstFalse
// (which are GCed by normalize_root() before we retry compilation)
// it is important to first make a pass using reorder_root() and then optimize_binops()
static ast_node *optimize_binops(ast_node *root, runtime_ctx &rctx)
{
	ast_node *normalize_root(ast_node *root); // in queries.cpp

        for (bool updates{false}; root; updates = false)
        {
                optimize_binops_impl(root, updates, rctx);

                if (updates)
                {
                        // 1+ nodes were modified
                        root = normalize_root(root);
                }
                else
                        break;
        }
        return root;
}


// Considers all binary ops, and potentiall swaps (lhs, rhs) of binary ops, but not based on actual cost
// but on heuristics .
// See optimize_binops(), which does a similar job, except it takes into account the cost to evaluate each branch
struct reorder_ctx
{
        bool dirty;
};

static void reorder(ast_node *n, reorder_ctx *const ctx)
{
        if (n->type == ast_node::Type::BinOp)
        {
                const auto lhs = n->binop.lhs, rhs = n->binop.rhs;

                reorder(lhs, ctx);
                reorder(rhs, ctx);

                if (n->binop.op == Operator::AND || n->binop.op == Operator::STRICT_AND)
                {
                        if (lhs->type == ast_node::Type::BinOp)
                        {
                                if (rhs->is_unary())
                                {
                                        n->binop.lhs = rhs;
                                        n->binop.rhs = lhs;

                                        ctx->dirty = true;
                                }
                        }
                }
                else if (n->binop.op == Operator::NOT)
                {
                        // (foo OR bar) NOT apple
                        // apple is cheaper to compute so we need to reverse those
                        if (rhs->is_unary() && lhs->type == ast_node::Type::BinOp)
                        {
                                auto llhs = lhs->binop.lhs;
                                auto lrhs = lhs->binop.rhs;

                                if (llhs->is_unary() && lrhs->type == ast_node::Type::BinOp && (lhs->binop.op == Operator::AND || lhs->binop.op == Operator::STRICT_AND))
                                {
                                        // ((pizza AND (sf OR "san francisco")) NOT onions)
                                        // => (pizza NOT onions) AND (sf OR "san francisco")
                                        const auto saved = lhs->binop.op;

                                        SLog("here\n");

                                        lhs->binop.rhs = rhs;
                                        lhs->binop.op = Operator::NOT;

                                        n->binop.op = saved;
                                        n->binop.rhs = lrhs;

                                        ctx->dirty = true;
                                }
                        }
                }
        }
}

static ast_node *reorder_root(ast_node *r)
{
        reorder_ctx ctx;

        do
        {
                ctx.dirty = false;
                reorder(r, &ctx);
        } while (ctx.dirty);

        return r;
}

static bool optimize(Trinity::query &q, runtime_ctx &rctx)
{
	reorder_root(q.root);
	q.root = optimize_binops(q.root, rctx);
	return q.root;
}







#pragma mark INTERPRETER

// TODO: consider passing exec_node& instead of exec_node
// to impl. so that can modify themselves if needed
typedef uint8_t (*node_impl)(exec_node, runtime_ctx &);

enum class OpCodes : uint8_t
{
        MatchToken = 0,
        LogicalAnd,
        LogicalOr,
        MatchPhrase,
	LogicalNot,
        UnaryAnd,
        UnaryNot,
        ConstFalse
};

static inline uint8_t eval(const exec_node node, runtime_ctx &ctx);


static inline uint8_t noop_impl(const exec_node, runtime_ctx &)
{
	return 0;
}

static inline uint8_t matchtoken_impl(const exec_node self, runtime_ctx &rctx)
{
	auto t = rctx.evalnode_contexts.tokens + self.nodeCtxIdx;
	auto decoder = rctx.decode_ctx.decoders[t->termID];
	const auto res = decoder->seek(rctx.curDocID);

	SLog(ansifmt::color_green, "Attempting to match token against ", rctx.curDocID, ansifmt::reset, " => ", res, "\n");
	return res;
}

#if 0
static inline uint8_t matchphrase_impl(const exec_node self, runtime_ctx &rctx)
{
	auto p = rctx.evalnode_contexts.phrases + self.nodeCtxIdx;
	const auto firstTermID = p->termIDs[0];
	auto decoder = rctx.decode_ctx.decoders[firstTermID];
	const auto did = rctx.curDocID;

	if (!decoder->seek(did))
		return 0;

	auto th = rctx.materialize_term_hits(firstTermID);
	const auto n = p->size;
	exec_term_id_t phrasePrevTermID{firstTermID};

	for (uint32_t i{1}; i < n; ++i)
	{
		const auto termID = p->termIDs[i];
		auto decoder = rctx.decode_ctx.decoders[termID];

		if (!decoder->seek(did))
			return 0;

		auto res = rctx.materialize_term_hits_with_phrase_prevterm_check(termID, phrasePrevTermID);

		if (!res.second)
			return 0;

		phrasePrevTermID = termID;
	}


	return 1;
}
#endif

static uint8_t matchphrase_impl(const exec_node self, runtime_ctx &rctx)
{
	static constexpr bool trace{true};
        auto p = rctx.evalnode_contexts.phrases + self.nodeCtxIdx;
        const auto firstTermID = p->termIDs[0];
        auto decoder = rctx.decode_ctx.decoders[firstTermID];
        const auto did = rctx.curDocID;

	if (trace)
		SLog("PHRASE CHECK document ", rctx.curDocID, "\n");


        if (!decoder->seek(did))
	{
		if (trace)
			SLog("Failed for first phrase token\n");
                return 0;
	}

        const auto n = p->size;

        for (uint32_t i{1}; i != n; ++i)
        {
                const auto termID = p->termIDs[i];
                auto decoder = rctx.decode_ctx.decoders[termID];

		if (trace)
			SLog("Phrase token ", i, " ", termID, "\n");

                if (!decoder->seek(did))
		{
			if (trace)
				SLog("Failed for phrase token\n");
                        return 0;
		}

		rctx.materialize_term_hits(termID);
        }

        auto th = rctx.materialize_term_hits(firstTermID);
        const auto firstTermFreq = th->freq;
        const auto firstTermHits = th->all;
        auto &dws = rctx.docWordsSpace;

        for (uint32_t i{0}; i != firstTermFreq; ++i)
        {
                if (const auto pos = firstTermHits[i].pos)
                {
			if (trace)
				SLog("<< POS ", pos, "\n");

                        for (uint8_t k{1};; ++k)
                        {
                                if (k == n)
                                {
                                        // matched seq
                                        return true;
                                }

                                const auto termID = p->termIDs[k];

                                if (!dws.test(termID, pos + k))
                                        break;
                        }
                }
        }

        return false;
}

static inline uint8_t logicaland_impl(const exec_node self, runtime_ctx &rctx)
{
	auto opctx = rctx.evalnode_contexts.binaryOps + self.nodeCtxIdx;

	return eval(opctx->lhs, rctx) && eval(opctx->rhs, rctx);
}

static inline uint8_t logicalnot_impl(const exec_node self, runtime_ctx &rctx)
{
	auto opctx = rctx.evalnode_contexts.binaryOps + self.nodeCtxIdx;

	return eval(opctx->lhs, rctx) && !eval(opctx->rhs, rctx);
}

static inline uint8_t logicalor_impl(const exec_node self, runtime_ctx &rctx)
{
	auto opctx = rctx.evalnode_contexts.binaryOps + self.nodeCtxIdx;

	return eval(opctx->lhs, rctx) || eval(opctx->rhs, rctx);
}


uint8_t inline eval(const exec_node node, runtime_ctx &ctx)
{
        static constexpr node_impl implementations[] =
            {
                matchtoken_impl,
                logicaland_impl,
                logicalor_impl,
		matchphrase_impl,
                logicalnot_impl,
                noop_impl,
                noop_impl,
                noop_impl,
            };

        return implementations[node.implIdx](node, ctx);
}





#pragma mark COMPILER
static exec_node compile(const ast_node *const n, runtime_ctx &ctx)
{
        exec_node res;

        res.flags = 0;
        require(n);
        switch (n->type)
        {
		case ast_node::Type::Dummy:
			std::abort();

                case ast_node::Type::Token:
                        res.implIdx = (unsigned)OpCodes::MatchToken;
                        res.nodeCtxIdx = ctx.register_token(n->p);
                        break;

                case ast_node::Type::Phrase:
                        if (n->p->size == 1)
                        {
                                res.implIdx = (unsigned)OpCodes::MatchToken;
                                res.nodeCtxIdx = ctx.register_token(n->p);
                        }
                        else
                        {
                                res.implIdx = (unsigned)OpCodes::MatchPhrase;
                                res.nodeCtxIdx = ctx.register_phrase(n->p);
                        }
                        break;

                case ast_node::Type::BinOp:
                        switch (n->binop.op)
                        {
                                case Operator::AND:
                                case Operator::STRICT_AND:
                                        res.implIdx = (unsigned)OpCodes::LogicalAnd;
                                        break;

                                case Operator::OR:
                                        res.implIdx = (unsigned)OpCodes::LogicalOr;
                                        break;

                                case Operator::NOT:
                                        res.implIdx = (unsigned)OpCodes::LogicalNot;
                                        break;

				case Operator::NONE:
					std::abort();
					break;
                        }
                        res.nodeCtxIdx = ctx.register_binop(compile(n->binop.lhs, ctx), compile(n->binop.rhs, ctx));
                        break;

                case ast_node::Type::ConstFalse:
                        res.implIdx = (unsigned)OpCodes::ConstFalse;
                        break;

                case ast_node::Type::UnaryOp:
                        switch (n->unaryop.op)
                        {
                                case Operator::AND:
                                case Operator::STRICT_AND:
                                        res.implIdx = (unsigned)OpCodes::UnaryAnd;
                                        break;

                                case Operator::NOT:
                                        res.implIdx = (unsigned)OpCodes::UnaryNot;
                                        break;

				default:
					std::abort();
                        }
                        res.nodeCtxIdx = ctx.register_unaryop(compile(n->unaryop.expr, ctx));
                        break;
        }

        return res;
}










// If we have multiple segments, we should invoke exec() for each of them
// in parallel or in sequence, collect the top X hits and then later merge them
//
// We will need to create a copy of the `q` after we have normalized it, and then
// we need to reorder and optimize that copy, get leaders and execute it -- for each segment, but
// this is a very fast operation anyway
bool Trinity::exec_query(const query &in, segment &seg,  dids_scanner_registry *const maskedDocumentsRegistry)
{
	if (!in)
	{
		SLog("No root node\n");
		return false;
	}

	// we need a copy of that query here
	// for we we will need to modify it
	query q(in);


	// Normalize just in case
	if (!q.normalize())
	{
		SLog("No root node after normalization\n");
		return false;
	}

	runtime_ctx rctx(&seg);

	// Optimizations we shouldn't perform on the parsed query because
	// the rewrite it by potentially moving nodes around or dropping nodes
	if (!optimize(q, rctx))
	{
		// After optimizations nothing's left
		SLog("No root node after optimizations\n");
		return false;
	}


	SLog("Compiling\n");
	// Need to compile before we access the leader nodes
	const auto rootExecNode = compile(q.root, rctx);


	// We need the leader nodes 
	// See leader_nodes() impl. comments
	std::vector<ast_node *> leaderNodes;
	uint16_t toAdvance[1024];
	Switch::vector<Trinity::Codecs::Decoder *> leaderTokensDecoders;


        {
                Switch::vector<strwlen8_t> leaderTokensV;

		q.leader_nodes(&leaderNodes);
                for (const auto n : leaderNodes)
                {
                        for (uint32_t i{0}; i != n->p->size; ++i)
                                leaderTokensV.push_back(n->p->terms[i].token);
                }

		Dexpect(leaderTokensV.size() < sizeof_array(toAdvance));

                std::sort(leaderTokensV.begin(), leaderTokensV.end(), [](const auto &a, const auto &b) {
                        return Text::StrnncasecmpISO88597(a.data(), a.size(), b.data(), b.size()) < 0;
                });

                leaderTokensV.resize(std::unique(leaderTokensV.begin(), leaderTokensV.end()) - leaderTokensV.begin());

                Print("leaderTokens:", leaderTokensV, "\n");

                for (const auto t : leaderTokensV)
                {
                        const auto termID = seg.resolve_term(t);
                        require(termID);
			SLog("Leader termID = ", termID, "\n");
                        auto decoder = rctx.decode_ctx.decoders[termID];
			require(decoder);
			

                        decoder->begin();
			leaderTokensDecoders.push_back(decoder);
                }
        }

	require(leaderTokensDecoders.size());
        // We can now compile the AST to an optimised bytecode representation
	auto leaderDecoders = leaderTokensDecoders.data();
	uint32_t leaderDecodersCnt = leaderTokensDecoders.size();

	SLog("RUNNING\n");



	rctx.setup_evalnode_contexts();


	// TODO: if (q.root->type == ast_node::Type::Token) {} 
	// i.e if just a single term was entered, scan that single token's documents  without even having to use a decoder
	// otherwise use the loop that tracks lead tokens
	for (;;)
        {
		// Select document from the leader tokens/decoders
                uint32_t docID = leaderDecoders[0]->cur_doc_id();
                uint8_t toAdvanceCnt{1};

                toAdvance[0] = 0;

                for (uint32_t i{1}; i < leaderDecodersCnt; ++i)
                {
                        const auto decoder = leaderDecoders[i];
                        const auto did = decoder->cur_doc_id();

                        if (did < docID)
                        {
                                docID = did;
                                toAdvance[0] = i;
                                toAdvanceCnt = 1;
                        }
                        else if (did == docID)
                                toAdvance[toAdvanceCnt++] = i;
                }

		SLog("DOCUMENT ", docID, "\n");


		if (!maskedDocumentsRegistry->test(docID))
		{
                        // now execute rootExecNode
                        // and it it returns true, compute the document's score
                        rctx.reset(docID);

                        const auto res = eval(rootExecNode, rctx);

                        if (res)
                                SLog(ansifmt::bold, ansifmt::color_blue, "MATCHED ", docID, ansifmt::reset, "\n");

		}



		// Advance leader tokens/decoders
		do
		{
                        const auto idx = toAdvance[--toAdvanceCnt];
                        auto decoder = leaderDecoders[idx];

                        if (!decoder->next())
                        {
                                // done with this leaf token
                                if (!--leaderDecodersCnt)
                                        goto l1;

                                memmove(leaderDecoders + idx, leaderDecoders + idx + 1, (leaderDecodersCnt - idx) * sizeof(Trinity::Codecs::Decoder *));
                        }

                } while (toAdvanceCnt);
        }

l1:;

        return true;
}
