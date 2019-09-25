#include <iostream>
#include <vector>
#include <fstream>
#include <set>
#include <algorithm>
#include <stdexcept>
#include <memory>

#include "core/por-bytecode.h"
#include "core/offset-map.h"

namespace soren {

struct SceneInfo
{
	unsigned idx;

	std::string name;
	unsigned kind;

	std::vector<int> parameters;

	unsigned argCnt;
	std::vector<std::string> varnames;

	bool isGlobal : 1;
};

struct ScriptInfo
{
	const char* get_cstr(unsigned offset) const
	{
		if (offset >= strpool.size())
			throw std::runtime_error("Bad string pool offset");

		return strpool.data() + offset;
	}

	std::vector<SceneInfo> scenes;
	std::vector<char> strpool;

	std::vector<std::string> globalnames; // TODO: maybe
};

static
std::vector<byte_type> read_entire_file(const char* filename)
{
	std::ifstream in(filename, std::ios::binary | std::ios::ate);

	if (!in.is_open())
		throw "couldn't open file for binary read"; // FIXME

	const auto size = in.tellg();
	std::vector<byte_type> result(size);

	in.seekg(0, std::ios::beg);
	in.read(reinterpret_cast<std::ifstream::char_type*>(result.data()), size);

	return result;
}

template<typename IteratorType, typename ResultType = std::uint32_t>
ResultType decode_int_le(IteratorType begin, IteratorType end)
{
	static_assert(std::is_convertible<typename std::iterator_traits<IteratorType>::value_type, byte_type>::value, "decode_le: expected byte (u8) iterators");

	ResultType result = 0;
	unsigned i = 0;

	while (begin != end)
		result = result + (*begin++ << (8*i++));

	return result;
}

template<typename IteratorType, typename ResultType = std::uint32_t>
ResultType decode_int_be(IteratorType begin, IteratorType end)
{
	static_assert(std::is_convertible<typename std::iterator_traits<IteratorType>::value_type, byte_type>::value, "decode_le: expected byte (u8) iterators");

	ResultType result = 0;

	while (begin != end)
		result = *begin++ + (result << 8);

	return result;
}

template<typename ResultType = std::uint32_t>
ResultType decode_int_le(Span<const byte_type> span)
{
	return decode_int_le<decltype(span.begin()), ResultType>(span.begin(), span.end());
}

template<typename ResultType = std::uint32_t>
ResultType decode_int_be(Span<const byte_type> span)
{
	return decode_int_be<decltype(span.begin()), ResultType>(span.begin(), span.end());
}

template<typename IntType = std::int32_t>
IntType sign_extend(IntType value, unsigned bits)
{
	static_assert(std::is_signed<IntType>::value, "Result of sign_extend should be signed");

	const auto rbits = (sizeof(IntType)*8 - bits);

	return (value << rbits) >> rbits;
}

struct DisEvent
{
	std::wstring name;
};

using DisIns = BcIns<unsigned>;

template<bool IsFE10 = true>
std::vector<DisIns> decode_script(Span<const byte_type> bytes)
{
	std::vector<DisIns> result;

	unsigned i = 0, lastJump = 0;
	bool ended = false;

	while (!ended && i < bytes.size())
	{
		DisIns ins { i, 0, 0 };
		ins.opcode = bytes[i++];

		if (!ins.valid<IsFE10>())
		{
			if (ins.valid<true>())
				throw std::runtime_error("Invalid FE9 opcode (only valid in FE10)."); // TODO: better error

			throw std::runtime_error("Invalid opcode."); // TODO: better error
		}

		if (ins.info().operandSize > 0)
		{
			if (i + ins.info().operandSize >= bytes.size())
				throw std::runtime_error("Reached end of script when expecting operand."); // TODO: better error

			ins.operand = decode_int_be(
				bytes.begin() + i,
				bytes.begin() + i + ins.info().operandSize);

			ins.operand = sign_extend(ins.operand, ins.info().operandSize*8);

			i += ins.info().operandSize;

			if (IsFE10 && (ins.opcode == BC_OPCODE_CALL))
			{
				// in FE10, call(37) has a variable length operand
				// if the first byte of the operand is >= 0x80
				// the operand will be 2 bytes be, with the top bit removed

				if (ins.operand & 0x80)
					ins.operand = ((ins.operand & 0x7F) << 8) + bytes[i++];
			}
		}

		switch (ins.opcode)
		{

		case BC_OPCODE_B:
		case BC_OPCODE_BY:
		case BC_OPCODE_BKY:
		case BC_OPCODE_BN:
		case BC_OPCODE_BKN:
			ins.operand = i + ins.operand - ins.info().operandSize;
			lastJump = std::max(lastJump, (unsigned) ins.operand);

			break;

		case BC_OPCODE_RETURN:
		case BC_OPCODE_RETN:
		case BC_OPCODE_RETY:
			if (i > lastJump)
				ended = true;

			break;

		} // switch (ins.opcode)

		result.push_back(ins);
	}

	return result;
}

template<bool IgnoreBranchAndKeeps = true>
OffsetMap<Span<const DisIns>> slice_script(Span<const DisIns> script)
{
	OffsetMap<Span<const DisIns>> result;
	std::set<std::size_t> slicePoints;

	// Step 1: Find slice points

	for (auto& ins : script)
	{
		if (IgnoreBranchAndKeeps)
		{
			if (ins.opcode == BC_OPCODE_BKN || ins.opcode == BC_OPCODE_BKY)
				continue;
		}

		if (ins.info().isJump)
		{
			// jumps generate:
			// a slice after themselves
			// a slice before the jump target
			// a label before the jump target

			slicePoints.insert(ins.location + 1 + ins.info().operandSize);
			slicePoints.insert(ins.operand);
		}

		if (ins.opcode == BC_OPCODE_RETURN)
		{
			// ends generate slices after themselves
			slicePoints.insert(ins.location + 1);
		}
	}

	// Step 2: Slice

	auto scrIt   = script.begin();
	auto sliceIt = slicePoints.begin();

	while (scrIt != script.end())
	{
		auto itStart = scrIt;

		if (sliceIt != slicePoints.end())
		{
			auto sliceOffset = *sliceIt++;

			scrIt = std::find_if(itStart, script.end(), [sliceOffset] (auto& ins)
			{
				return ins.location >= sliceOffset;
			});
		}
		else
		{
			scrIt = script.end();
		}

		result.set(itStart->location, { itStart, scrIt });
	}

	return result;
}

Span<DisIns> convert_bks_to_fake_logic(Span<DisIns> slice)
{
	// Converts bky/bkn chains to fake land/lorr instructions and reorder accordingly
	// ex:
	/*
	 * 0 val 0
	 * 2 bkn 7
	 * 5 val 1
	 * 7 bn ...
	 */
	// becomes
	/*
	 * 0 val 0
	 * 5 val 1
	 * 2 fake!land
	 * 7 bn ...
	 */

	for (unsigned i = 0; i < slice.size(); ++i)
	{
		unsigned op = slice[i].opcode;

		switch (op)
		{

		case BC_OPCODE_BKN:
		case BC_OPCODE_BKY:
		{
			unsigned target = slice[i++].operand;
			unsigned j = i;

			while (j < slice.size() && slice[j].location != target)
			{
				std::swap(slice[j-1], slice[j]);
				j++;
			}

			slice[j-1].opcode = (op == BC_OPCODE_BKN) ? BC_FAKEOP_LAND : BC_FAKEOP_LORR;
			slice[j-1].operand = 0;
		}

		default:
			continue;

		} // switch (op)
	}

	return slice;
}

std::vector<DisIns> get_bks_as_fake_logic(Span<const DisIns> slice)
{
	std::vector<DisIns> result(slice.begin(), slice.end());
	convert_bks_to_fake_logic(result);

	return result;
}

struct Expr
{
	enum class Kind
	{
		// No children
		IntLiteral,
		StrLiteral,
		Named,

		// One child (unary operators)
		Neg,
		Not,
		BitwiseNot,
		Deref, // value-at-address ([])
		Addrof, // address-of-value (&)

		// Two child (binary operators)
		Assign,
		Add,
		Sub,
		Mul,
		Div,
		Mod,
		Or,
		And,
		Xor,
		Lsl,
		Lsr,
		Eq,
		Ne,
		Lt, // Maybe
		Le,
		Gt, // Maybe
		Ge, // Maybe
		EqStr,
		NeStr,
		LogicalAnd,
		LogicalOr,

		Func, // Name + Variable Children

		Invalid,
	};

	Kind kind { Kind::Invalid };

	// TODO: embed expression source location? (either bytecode offset or file:line:col)
	// TODO (C++17): use std::variant

	// Literal
	std::int32_t literal {};

	// Named/String/FnName
	std::string named;

	std::vector<std::unique_ptr<Expr>> children;

	static inline
	std::unique_ptr<Expr> make_unique_intlit(std::int32_t value)
	{
		std::unique_ptr<Expr> result = std::make_unique<Expr>();

		result->kind = Kind::IntLiteral;
		result->literal = value;

		return result;
	}

	static inline
	std::unique_ptr<Expr> make_unique_strlit(std::string&& value)
	{
		std::unique_ptr<Expr> result = std::make_unique<Expr>();

		result->kind = Kind::StrLiteral;
		result->named = std::move(value);

		return result;
	}

	static inline
	std::unique_ptr<Expr> make_unique_identifier(std::string&& value)
	{
		std::unique_ptr<Expr> result = std::make_unique<Expr>();

		result->kind = Kind::Named;
		result->named = std::move(value);

		return result;
	}

	static inline
	std::unique_ptr<Expr> make_unique_unop(Kind kind, std::unique_ptr<Expr>&& inner)
	{
		std::unique_ptr<Expr> result = std::make_unique<Expr>();

		result->kind = kind;
		result->children.push_back(std::move(inner));

		return result;
	}

	static inline
	std::unique_ptr<Expr> make_unique_binop(Kind kind, std::unique_ptr<Expr>&& lexpr, std::unique_ptr<Expr>&& rexpr)
	{
		std::unique_ptr<Expr> result = std::make_unique<Expr>();

		result->kind = kind;
		result->children.push_back(std::move(lexpr));
		result->children.push_back(std::move(rexpr));

		return result;
	}

	static inline
	std::unique_ptr<Expr> make_unique_copy(const Expr& expr)
	{
		std::unique_ptr<Expr> result = std::make_unique<Expr>();

		result->kind = expr.kind;
		result->literal = expr.literal;
		result->named = expr.named;

		for (auto& child : expr.children)
			result->children.push_back(make_unique_copy(*child));

		return result;
	}

};

struct Stmt
{
	enum class Kind
	{
		Push, // One child expr
		Expr, // One child expr
		Goto, // One child constant expr
		GotoIf, // Two child exprs (first constant)
		Yield, // No children
		Return, // One child expr
	};

	Kind kind;

	std::string label; //< TODO: better

	std::vector<std::unique_ptr<Expr>> children;

	static inline
	Stmt make_push(std::unique_ptr<Expr>&& inner)
	{
		Stmt result { Kind::Push, {}, {} };

		result.children.push_back(std::move(inner));

		return result;
	}

	static inline
	Stmt make_goto(std::int32_t target)
	{
		Stmt result { Kind::Goto, {}, {} };

		// FIXME: use (or "register") actual label names instead of generating some on the fly

		result.children.push_back(Expr::make_unique_identifier([&] ()
		{
			std::string name;
			name.append("label_");
			name.append(std::to_string(target));
			return name;
		} ()));

		return result;
	}

	static inline
	Stmt make_goto_if(std::int32_t target, std::unique_ptr<Expr>&& truth)
	{
		Stmt result { Kind::GotoIf, {}, {} };

		// FIXME: do not copy-paste this and reuse same code as in make_goto

		result.children.push_back(Expr::make_unique_identifier([&] ()
		{
			std::string name;
			name.append("label_");
			name.append(std::to_string(target));
			return name;
		} ()));

		result.children.push_back(std::move(truth));

		return result;
	}

	static inline
	Stmt make_yield(void)
	{
		return Stmt { Kind::Yield, {}, {} };
	}

	static inline
	Stmt make_return(std::unique_ptr<Expr>&& inner)
	{
		Stmt result { Kind::Return, {}, {} };

		result.children.push_back(std::move(inner));

		return result;
	}
};

std::vector<Stmt> make_statements(const ScriptInfo& script, const SceneInfo& scene, Span<const DisIns> slice)
{
	std::vector<Stmt> result;
	result.reserve(slice.size());

	const auto expect_push = [&] (const char*, auto func)
	{
		if (result.size() < 1)
			throw std::runtime_error("expected after push"); // TODO: better error ("name" only expected after push)

		if (result.back().kind != Stmt::Kind::Push)
			throw std::runtime_error("expected after push"); // TODO: better error ("name" only expected after push)

		func(result.back());
	};

	const auto expect_push_push = [&] (const char*, auto func)
	{
		if (result.size() < 2)
			throw false; // FIXME: error ("name" as first instruction)

		if (result.back().kind != Stmt::Kind::Push)
			throw false; // FIXME: error ("name" only expected after 2 pushes)

		auto& rop = result.back();

		if (result[result.size()-2].kind != Stmt::Kind::Push)
			throw false; // FIXME: error ("name" only expected after 2 pushes)

		auto& lop = result[result.size()-2];

		func(lop, rop);
	};

	const auto unop = [&] (const char* name, Expr::Kind kind)
	{
		expect_push(name, [&] (auto& back)
		{
			back.children[0] = Expr::make_unique_unop(kind, std::move(back.children[0]));
		});
	};

	const auto binop = [&] (const char* name, Expr::Kind kind)
	{
		expect_push_push(name, [&] (auto& l, auto& r)
		{
			auto lexpr = std::move(l.children[0]);
			auto rexpr = std::move(r.children[0]);

			result.pop_back();
			result.pop_back();

			result.push_back(Stmt::make_push(
				Expr::make_unique_binop(kind, std::move(lexpr), std::move(rexpr))));
		});
	};

	const auto call = [&] (const char* funcname, unsigned argCnt)
	{
		if (result.size() < argCnt)
			throw false; // FIXME: error (call expected after x pushes)

		for (unsigned i = result.size() - argCnt; i < result.size(); ++i)
			if (result[i].kind != Stmt::Kind::Push)
				throw false; // FIXME: error (call expexted after x pushes)

		auto callexpr = std::make_unique<Expr>();

		callexpr->kind = Expr::Kind::Func;
		callexpr->named = funcname;

		for (unsigned i = result.size() - argCnt; i < result.size(); ++i)
			callexpr->children.push_back(std::move(result[i].children[0]));

		result.resize(result.size() - argCnt);
		result.push_back(Stmt::make_push(std::move(callexpr)));
	};

	for (auto& ins : slice)
	{
		switch (ins.opcode)
		{

		case BC_OPCODE_NOP:
			// nothing

			break;

		case BC_OPCODE_VAL8:
		case BC_OPCODE_VAL16:
			// push varname

			result.push_back(Stmt::make_push(
				Expr::make_unique_identifier(std::string(scene.varnames[ins.operand]))));

			break;

		case BC_OPCODE_VALX8:
		case BC_OPCODE_VALX16:
			// push a => push [&varname + a]

			expect_push("valx", [&] (auto& back)
			{
				back.children[0] = Expr::make_unique_unop(Expr::Kind::Deref,
					Expr::make_unique_binop(Expr::Kind::Add,
						Expr::make_unique_unop(Expr::Kind::Addrof,
							Expr::make_unique_identifier(std::string(scene.varnames[ins.operand]))),
						std::move(back.children[0])));
			});

			break;

		case BC_OPCODE_REF8:
		case BC_OPCODE_REF16:
			// push &varname

			result.push_back(Stmt::make_push(
				Expr::make_unique_unop(Expr::Kind::Addrof,
					Expr::make_unique_identifier(std::string(scene.varnames[ins.operand])))));

			break;

		case BC_OPCODE_REFX8:
		case BC_OPCODE_REFX16:
			// push a => push &varname + a

			expect_push("refx", [&] (auto& back)
			{
				back.children[0] = Expr::make_unique_binop(Expr::Kind::Add,
					Expr::make_unique_unop(Expr::Kind::Addrof,
						Expr::make_unique_identifier(std::string(scene.varnames[ins.operand]))),
					std::move(back.children[0]));
			});

			break;

		case BC_OPCODE_GVAL8:
		case BC_OPCODE_GVAL16:
			// push varname

			result.push_back(Stmt::make_push(
				Expr::make_unique_identifier(std::string(script.globalnames[ins.operand]))));

			break;

		case BC_OPCODE_GVALX8:
		case BC_OPCODE_GVALX16:
			// push a => push [&varname + a]

			expect_push("valx", [&] (auto& back)
			{
				back.children[0] = Expr::make_unique_unop(Expr::Kind::Deref,
					Expr::make_unique_binop(Expr::Kind::Add,
						Expr::make_unique_unop(Expr::Kind::Addrof,
							Expr::make_unique_identifier(std::string(script.globalnames[ins.operand]))),
						std::move(back.children[0])));
			});

			break;

		case BC_OPCODE_GREF8:
		case BC_OPCODE_GREF16:
			// push &varname

			result.push_back(Stmt::make_push(
				Expr::make_unique_unop(Expr::Kind::Addrof,
					Expr::make_unique_identifier(std::string(script.globalnames[ins.operand])))));

			break;

		case BC_OPCODE_GREFX8:
		case BC_OPCODE_GREFX16:
			// push a => push &varname + a

			expect_push("refx", [&] (auto& back)
			{
				back.children[0] = Expr::make_unique_binop(Expr::Kind::Add,
					Expr::make_unique_unop(Expr::Kind::Addrof,
						Expr::make_unique_identifier(std::string(script.globalnames[ins.operand]))),
					std::move(back.children[0]));
			});

			break;

		case BC_OPCODE_NUMBER8:
		case BC_OPCODE_NUMBER16:
		case BC_OPCODE_NUMBER32:
			// push imm

			result.push_back(Stmt::make_push(
				Expr::make_unique_intlit(ins.operand)));

			break;

		case BC_OPCODE_STRING8:
		case BC_OPCODE_STRING16:
		case BC_OPCODE_STRING32:
			// push <string at imm>

			result.push_back(Stmt::make_push(
				Expr::make_unique_strlit({ script.get_cstr(ins.operand) })));

			break;

		case BC_OPCODE_DEREF:
			// push a => push a, [a]

			expect_push("deref", [&] (auto& back)
			{
				result.push_back(Stmt::make_push(
					Expr::make_unique_unop(Expr::Kind::Deref,
						Expr::make_unique_copy(*back.children[0]))));
			});

			break;

		case BC_OPCODE_DISC:
			// push a => a

			expect_push("disc", [&] (auto& back)
			{
				back.kind = Stmt::Kind::Expr;
			});

			break;

		case BC_OPCODE_STORE:
			// push a, b => push [a] = b

			binop("store", Expr::Kind::Assign);
			break;

		case BC_OPCODE_ADD:
			// push a, b => push a + b

			binop("add", Expr::Kind::Add);
			break;

		case BC_OPCODE_SUB:
			// push a, b => push a - b

			binop("sub", Expr::Kind::Sub);
			break;

		case BC_OPCODE_MUL:
			// push a, b => push a * b

			binop("mul", Expr::Kind::Mul);
			break;

		case BC_OPCODE_DIV:
			// push a, b => push a / b

			binop("div", Expr::Kind::Div);
			break;

		case BC_OPCODE_MOD:
			// push a, b => push a % b

			binop("mod", Expr::Kind::Mod);
			break;

		case BC_OPCODE_ORR:
			// push a, b => push a | b

			binop("orr", Expr::Kind::Or);
			break;

		case BC_OPCODE_AND:
			// push a, b => push a & b

			binop("and", Expr::Kind::And);
			break;

		case BC_OPCODE_XOR:
			// push a, b => push a ^ b

			binop("xor", Expr::Kind::Xor);
			break;

		case BC_OPCODE_LSL:
			// push a, b => push a << b

			binop("lsl", Expr::Kind::Lsl);
			break;

		case BC_OPCODE_LSR:
			// push a, b => push a >> b

			binop("lsr", Expr::Kind::Lsr);
			break;

		case BC_OPCODE_EQ:
			// push a, b => push a == b

			binop("eq", Expr::Kind::Eq);
			break;

		case BC_OPCODE_NE:
			// push a, b => push a != b

			binop("ne", Expr::Kind::Ne);
			break;

		case BC_OPCODE_LT:
			// push a, b => push a < b

			binop("lt", Expr::Kind::Lt);
			break;

		case BC_OPCODE_LE:
			// push a, b => push a <= b

			binop("le", Expr::Kind::Le);
			break;

		case BC_OPCODE_GT:
			// push a, b => push a > b

			binop("gt", Expr::Kind::Gt);
			break;

		case BC_OPCODE_GE:
			// push a, b => push a >= b

			binop("ge", Expr::Kind::Ge);
			break;

		case BC_OPCODE_EQSTR:
			// push a, b => push a <=> b

			binop("eqstr", Expr::Kind::EqStr);
			break;

		case BC_OPCODE_NESTR:
			// push a, b => push a <!> b

			binop("nestr", Expr::Kind::NeStr);
			break;

		case BC_OPCODE_NEG:
			// push a => push -a

			unop("neg", Expr::Kind::Neg);
			break;

		case BC_OPCODE_NOT:
			// push a => push !a

			unop("not", Expr::Kind::Not);
			break;

		case BC_OPCODE_MVN:
			// push a => push ~a

			unop("mvn", Expr::Kind::BitwiseNot);
			break;

		case BC_OPCODE_CALL:
			// push ... => push func(...)

			call(script.scenes[ins.operand].name.c_str(), script.scenes[ins.operand].argCnt);
			break;

		case BC_OPCODE_CALLEXT:
			// push ... => push func(...)

			call(script.get_cstr(ins.operand >> 8), ins.operand & 0xFF);
			break;

		case BC_OPCODE_RETURN:
			// push a => return a

			expect_push("ret", [&] (auto& back)
			{
				back.kind = Stmt::Kind::Return;
			});

			break;

		case BC_OPCODE_B:
			// goto off

			result.push_back(Stmt::make_goto(ins.operand));
			break;

		case BC_OPCODE_BN:
			// push a => goto off if !a

			expect_push("bn", [&] (auto& back)
			{
				auto expr = std::move(back.children[0]);
				result.pop_back();

				result.push_back(Stmt::make_goto_if(ins.operand,
					Expr::make_unique_unop(Expr::Kind::Not, std::move(expr))));
			});

			break;

		case BC_OPCODE_BY:
			// push a => goto off if a

			expect_push("by", [&] (auto& back)
			{
				auto expr = std::move(back.children[0]);
				result.pop_back();

				result.push_back(Stmt::make_goto_if(ins.operand, std::move(expr)));
			});

			break;

		case BC_OPCODE_YIELD:
			// yield

			result.push_back(Stmt::make_yield());
			break;

		case BC_OPCODE_40:
			// nothing

			break;

		case BC_OPCODE_PRINTF:
			// push ... => __printf(...)

			call("__printf", ins.operand);
			result.back().kind = Stmt::Kind::Expr;

			break;

		case BC_OPCODE_DUP:
			// push a => push a, a

			expect_push("dup", [&] (auto& back)
			{
				result.push_back(Stmt::make_push(
					Expr::make_unique_copy(*back.children[0])));
			});

			break;

		case BC_OPCODE_RETN:
			// return 0

			result.push_back(Stmt::make_return(
				Expr::make_unique_intlit(0)));

			break;

		case BC_OPCODE_RETY:
			// return 1

			result.push_back(Stmt::make_return(
				Expr::make_unique_intlit(1)));

			break;

		case BC_OPCODE_ASSIGN:
			// push a, b => [a] = b

			binop("assign", Expr::Kind::Assign);
			result.back().kind = Stmt::Kind::Expr;

			break;

		case BC_FAKEOP_LAND:
			// push a, b => push a && b

			binop("fake!land", Expr::Kind::LogicalAnd);
			break;

		case BC_FAKEOP_LORR:
			// push a, b => push a || b

			binop("fake!lorr", Expr::Kind::LogicalOr);
			break;

		default:
			throw false; // FIXME: unsupported opcode

		} // switch (ins.opcode)
	}

	return result;
}

std::ostream& operator << (std::ostream& os, const Expr& expr)
{
	switch (expr.kind)
	{

	case Expr::Kind::IntLiteral:
		return os << std::dec << expr.literal;

	case Expr::Kind::StrLiteral:
		return os << "\"" << expr.named << "\"";

	case Expr::Kind::Named:
		return os << expr.named;

	case Expr::Kind::Deref:
		return os << "[" << *expr.children[0] << "]";

	case Expr::Kind::Addrof:
		return os << "&" << *expr.children[0];

	case Expr::Kind::Assign:
		return os << "[" << *expr.children[0] << "] = " << *expr.children[1];

	case Expr::Kind::Add:
		return os << *expr.children[0] << " + " << *expr.children[1];

	case Expr::Kind::Sub:
		return os << *expr.children[0] << " - " << *expr.children[1];

	case Expr::Kind::Mul:
		return os << *expr.children[0] << " * " << *expr.children[1];

	case Expr::Kind::Div:
		return os << *expr.children[0] << " / " << *expr.children[1];

	case Expr::Kind::Mod:
		return os << *expr.children[0] << " % " << *expr.children[1];

	case Expr::Kind::And:
		return os << *expr.children[0] << " & " << *expr.children[1];

	case Expr::Kind::Or:
		return os << *expr.children[0] << " | " << *expr.children[1];

	case Expr::Kind::Xor:
		return os << *expr.children[0] << " ^ " << *expr.children[1];

	case Expr::Kind::Lsl:
		return os << *expr.children[0] << " << " << *expr.children[1];

	case Expr::Kind::Lsr:
		return os << *expr.children[0] << " >> " << *expr.children[1];

	case Expr::Kind::Not:
		return os << "!" << *expr.children[0];

	case Expr::Kind::Neg:
		return os << "-" << *expr.children[0];

	case Expr::Kind::BitwiseNot:
		return os << "~" << *expr.children[0];

	case Expr::Kind::Eq:
		return os << *expr.children[0] << " == " << *expr.children[1];

	case Expr::Kind::Ne:
		return os << *expr.children[0] << " != " << *expr.children[1];

	case Expr::Kind::Lt:
		return os << *expr.children[0] << " <? " << *expr.children[1];

	case Expr::Kind::Le:
		return os << *expr.children[0] << " <= " << *expr.children[1];

	case Expr::Kind::Gt:
		return os << *expr.children[0] << " >? " << *expr.children[1];

	case Expr::Kind::Ge:
		return os << *expr.children[0] << " >=? " << *expr.children[1];

	case Expr::Kind::EqStr:
		return os << *expr.children[0] << " <=> " << *expr.children[1];

	case Expr::Kind::NeStr:
		return os << *expr.children[0] << " <!> " << *expr.children[1];

	case Expr::Kind::LogicalAnd:
		return os << *expr.children[0] << " && " << *expr.children[1];

	case Expr::Kind::LogicalOr:
		return os << *expr.children[0] << " || " << *expr.children[1];

	case Expr::Kind::Func:
		os << expr.named << "(";

		for (unsigned i = 0; i < expr.children.size(); ++i)
		{
			if (i != 0)
				os << ", ";

			os << *expr.children[i];
		}

		return os << ")";

	default:
		return os << "<expr>";

	} // switch (expr.kind)
}

std::ostream& operator << (std::ostream& os, const Stmt& stmt)
{
	switch (stmt.kind)
	{

	case Stmt::Kind::Push:
		return os << "push " << *stmt.children[0] << ";";

	case Stmt::Kind::Expr:
		return os << *stmt.children[0] << ";";

	case Stmt::Kind::Return:
		return os << "return " << *stmt.children[0] << ";";

	case Stmt::Kind::Goto:
		return os << "goto " << *stmt.children[0] << ";";

	case Stmt::Kind::GotoIf:
		return os << "goto " << *stmt.children[0] << " if " << *stmt.children[1] << ";";

	case Stmt::Kind::Yield:
		return os << "yield;";

	} // switch (stmt.kind)
}

} // namespace soren

#include <iomanip>

int main(int argc, char** argv)
{
	if (argc < 2)
		return 1;

	std::string filename = argv[1];

	const auto data = soren::read_entire_file(filename.c_str());
	const auto span = soren::Span<const soren::byte_type>(data);

	const auto offStrings = soren::decode_int_le(span.subspan(0x24, 4));
	const auto offEvents  = soren::decode_int_le(span.subspan(0x28, 4));

	// std::cout << "SOME MEM SIZE: " << std::hex << soren::decode_int_le(span.subspan(0x22, 2)) << std::endl;

	soren::ScriptInfo scriptInfo;

	if (offStrings > offEvents)
		scriptInfo.strpool.assign(span.begin() + offStrings, span.end());
	else
		scriptInfo.strpool.assign(span.begin() + offStrings, span.begin() + offEvents);

	scriptInfo.globalnames.resize(soren::decode_int_le(span.subspan(0x22, 2)));

	for (unsigned i = 0; i < scriptInfo.globalnames.size(); ++i)
	{
		scriptInfo.globalnames[i] = [&] ()
		{
			std::string r("glob_");
			r.append(std::to_string(i));
			return r;
		} ();

		std::cout << "VARIABLE "<< scriptInfo.globalnames[i] << ";" << std::endl;
	}

	if (scriptInfo.globalnames.size() > 0)
		std::cout << std::endl;

	for (unsigned i = 0; const auto offEvent = soren::decode_int_le(span.subspan(offEvents+i*4, 4)); ++i)
	{
		scriptInfo.scenes.emplace_back();
		auto& scene = scriptInfo.scenes.back();

		const auto nameOff = soren::decode_int_le(span.subspan(offEvent+0x00, 4));

		scene.idx  = i;
		scene.argCnt = soren::decode_int_le(span.subspan(offEvent+0xD, 1));

		scene.name =  [&] () -> std::string
		{
			if (nameOff == 0)
			{
				return [&] ()
				{
					std::string r;
					r.append("unk_");
					r.append(std::to_string(i));
					return r;
				} ();
			}
			else
			{
				return [&] ()
				{
					std::string r;

					for (unsigned i = nameOff; span[i]; ++i)
						r.push_back(span[i]);

					return r;
				} ();
			}
		} ();

		scene.varnames.resize(soren::decode_int_le(span.subspan(offEvent+0x12, 2)));

		for (unsigned j = 0; j < scene.varnames.size(); ++j)
		{
			if (j < scene.argCnt)
				scene.varnames[j] = [&] () { std::string r("arg_"); r.append(std::to_string(j)); return r; } ();
			else
				scene.varnames[j] = [&] () { std::string r("var_"); r.append(std::to_string(j - scene.argCnt)); return r; } ();
		}

		scene.isGlobal = (nameOff != 0);

		scriptInfo.scenes.back().kind = soren::decode_int_le(span.subspan(offEvent+0x0C, 1));
	}

	for (unsigned i = 0; const auto offEvent = soren::decode_int_le(span.subspan(offEvents+i*4, 4)); ++i)
	{
		std::cout << "EVENT " << scriptInfo.scenes[i].name << "(";

		for (unsigned j = 0; j < scriptInfo.scenes[i].argCnt; ++j)
		{
			if (j != 0)
				std::cout << ", ";

			std::cout << scriptInfo.scenes[i].varnames[j];
		}

		std::cout << ")";

		if (scriptInfo.scenes[i].isGlobal)
			std::cout << " global";

		std::cout << std::endl;

		std::cout << "{" << std::endl;

		const auto offScript = soren::decode_int_le(span.subspan(offEvent+0x04, 4));
		const auto script = soren::decode_script(span.subspan(offScript));
		const auto slices = soren::slice_script(script);

		const auto labels = [&] ()
		{
			soren::NameMap result;

			for (auto& slice : slices)
			{
				for (auto& ins : slice.second)
				{
					if (ins.info().isJump)
						result.set(ins.operand, [&] () { std::string r("label_"); r.append(std::to_string(ins.operand)); return r; } ());
				}
			}

			return result;
		} ();

		for (auto& slice : slices)
		{
			if (slice.second.empty())
				continue;

			if (slice.first != 0)
				std::cout << std::endl;

			labels.for_at(slice.first, [&] (auto& name)
			{
				std::cout << name << ":" << std::endl;
			});

			// TODO: check whether any bkn/bky jumps to another slice, because that would be bad
			const auto fixedSlice = soren::get_bks_as_fake_logic(slice.second);

			for (auto& stmt : soren::make_statements(scriptInfo, scriptInfo.scenes[i], fixedSlice))
				std::cout << "  " << stmt << std::endl;
		}

		std::cout << "}" << std::endl << std::endl;
	}

	return 0;
}
