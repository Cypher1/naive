#include <stdio.h>
#include "array.h"
#include "ir_gen.h"
#include "misc.h"
#include "tokenise.h"
#include "parse.h"

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <input file>\n", argv[0]);
		return 1;
	}

	char *input_filename = argv[1];

	Array(SourceToken) tokens;
	tokenise(&tokens, input_filename);

	for (u32 i = 0; i < tokens.size; i++) {
		SourceToken *source_token = ARRAY_REF(&tokens, SourceToken, i);
		u32 line = source_token->source_loc.line;
		u32 column = source_token->source_loc.column;

		printf("%d:%d, %d\n", line, column, source_token->token.type);

		TokenType type = source_token->token.type;
		if (type == TOK_SYMBOL || type == TOK_STRING_LITERAL)
			printf("\t%s\n", source_token->token.val.symbol_or_string_literal);
	}

	ASTToplevel *ast = parse_toplevel(&tokens);
	if (ast == NULL)
		return 1;

	dump_toplevel(ast);
	TransUnit tu;
	trans_unit_init(&tu);
	Builder builder;
	builder_init(&builder);

	ir_gen_function(&tu, &builder, ast);

	putchar('\n');

	dump_trans_unit(&tu);

	return 0;
}
