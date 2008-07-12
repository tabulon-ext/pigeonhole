#include "lib.h"
#include "array.h"
#include "mail-storage.h"

#include "sieve-code.h"
#include "sieve-extensions.h"
#include "sieve-commands.h"
#include "sieve-result.h"
#include "sieve-validator.h" 
#include "sieve-generator.h"
#include "sieve-interpreter.h"
#include "sieve-actions.h"

#include "ext-imapflags-common.h"

static bool tag_flags_validate_persistent
	(struct sieve_validator *validator, struct sieve_command_context *cmd);
static bool tag_flags_validate
	(struct sieve_validator *validator,	struct sieve_ast_argument **arg, 
		struct sieve_command_context *cmd);
static bool tag_flags_generate
	(struct sieve_generator *generator, struct sieve_ast_argument *arg,
		struct sieve_command_context *cmd);

/* Tag */

const struct sieve_argument tag_flags = { 
	"flags", 
	NULL, NULL,
	tag_flags_validate, 
	NULL, 
	tag_flags_generate 
};

const struct sieve_argument tag_flags_implicit = { 
	"@flags-implicit", 
	NULL, 
	tag_flags_validate_persistent,
	NULL,	NULL, 
	tag_flags_generate 
};

/* Side effect */

extern const struct sieve_side_effect_extension imapflags_seffect_extension;

static bool seff_flags_dump_context
	(const struct sieve_side_effect *seffect,
    	const struct sieve_dumptime_env *denv, sieve_size_t *address);
static bool seff_flags_read_context
	(const struct sieve_side_effect *seffect, 
		const struct sieve_runtime_env *renv, sieve_size_t *address,
		void **se_context);
static bool seff_flags_read_implicit_context
	(const struct sieve_side_effect *seffect, 
		const struct sieve_runtime_env *renv, sieve_size_t *address,
		void **se_context);
static void seff_flags_print
	(const struct sieve_side_effect *seffect,	const struct sieve_action *action, 
		void *se_context, bool *keep);
static bool seff_flags_post_execute
	(const struct sieve_side_effect *seffect, const struct sieve_action *action, 
		const struct sieve_action_exec_env *aenv, 
		void *se_context, void *tr_context);

enum ext_imapflags_side_effect_type {
	EXT_IMAPFLAGS_SEFFECT_FLAGS,
	EXT_IMAPFLAGS_SEFFECT_FLAGS_IMPLICIT,
};

const struct sieve_side_effect flags_side_effect = {
	"flags",
	&act_store,
	
	&imapflags_seffect_extension,
	EXT_IMAPFLAGS_SEFFECT_FLAGS,
	seff_flags_dump_context,
	seff_flags_read_context,
	seff_flags_print,
	NULL,
	seff_flags_post_execute, 
	NULL, NULL
};

const struct sieve_side_effect flags_implicit_side_effect = {
	"flags-implicit",
	&act_store,
	
	&imapflags_seffect_extension,
	EXT_IMAPFLAGS_SEFFECT_FLAGS_IMPLICIT,
	NULL,
	seff_flags_read_implicit_context,
	seff_flags_print,
	NULL,
	seff_flags_post_execute, 
	NULL, NULL
};

const struct sieve_side_effect *imapflags_side_effects[] = {
	&flags_side_effect, &flags_implicit_side_effect
};

const struct sieve_side_effect_extension imapflags_seffect_extension = {
	&imapflags_extension,

	SIEVE_EXT_DEFINE_SIDE_EFFECTS(imapflags_side_effects)
};

/* Tag validation */

static bool tag_flags_validate_persistent
(struct sieve_validator *validator ATTR_UNUSED, struct sieve_command_context *cmd)
{	
	if ( sieve_command_find_argument(cmd, &tag_flags) == NULL ) {
		sieve_command_add_dynamic_tag(cmd, &tag_flags_implicit, -1);
	}
	
	return TRUE;
}

static bool tag_flags_validate
(struct sieve_validator *validator,	struct sieve_ast_argument **arg, 
	struct sieve_command_context *cmd)
{
	struct sieve_ast_argument *tag = *arg;

	/* Detach the tag itself */
	*arg = sieve_ast_argument_next(*arg);
	
	/* Check syntax:
	 *   :flags <list-of-flags: string-list>
	 */
	if ( !sieve_validate_tag_parameter
		(validator, cmd, tag, *arg, SAAT_STRING_LIST) ) {
		return FALSE;
	}
	
	tag->parameters = *arg;
	
	/* Detach parameter */
	*arg = sieve_ast_arguments_detach(*arg,1);

	return TRUE;
}

/* Tag generation */

static bool tag_flags_generate
(struct sieve_generator *generator, struct sieve_ast_argument *arg,
	struct sieve_command_context *cmd)
{
	struct sieve_ast_argument *param;
  struct sieve_binary *sbin = sieve_generator_get_binary(generator);

  if ( sieve_ast_argument_type(arg) != SAAT_TAG ) {
      return FALSE;
  }

	if ( arg->argument == &tag_flags ) {
		sieve_opr_side_effect_emit(sbin, &flags_side_effect, ext_imapflags_my_id);
		  
		param = arg->parameters;
	
		/* Call the generation function for the argument */ 
		if ( param->argument != NULL && param->argument->generate != NULL && 
			!param->argument->generate(generator, param, cmd) ) 
			return FALSE;
	} else if ( arg->argument == &tag_flags_implicit ) {
		sieve_opr_side_effect_emit
			(sbin, &flags_implicit_side_effect, ext_imapflags_my_id);
	} else
		return FALSE;
	
  return TRUE;
}

/* Side effect execution */

struct seff_flags_context {
	const char *const *keywords;
	enum mail_flags flags;
};

static bool seff_flags_dump_context
(const struct sieve_side_effect *seffect ATTR_UNUSED, 
	const struct sieve_dumptime_env *denv, sieve_size_t *address)
{
	return sieve_opr_stringlist_dump(denv, address);
}

static bool seff_flags_read_context
(const struct sieve_side_effect *seffect ATTR_UNUSED, 
	const struct sieve_runtime_env *renv, sieve_size_t *address,
	void **se_context)
{
	bool result = TRUE;
	pool_t pool = sieve_result_pool(renv->result);
	struct seff_flags_context *ctx;
	ARRAY_DEFINE(keywords, const char *);
	string_t *flags_item;
	struct sieve_coded_stringlist *flag_list;
	
	ctx = p_new(pool, struct seff_flags_context, 1);
	p_array_init(&keywords, pool, 2);
	
	t_push();
	
	/* Read flag-list */
	if ( (flag_list=sieve_opr_stringlist_read(renv, address)) == NULL ) {
		t_pop();
		return FALSE;
	}
	
	/* Unpack */
	flags_item = NULL;
	while ( (result=sieve_coded_stringlist_next_item(flag_list, &flags_item)) && 
		flags_item != NULL ) {
		const char *flag;
		struct ext_imapflags_iter flit;

		ext_imapflags_iter_init(&flit, flags_item);
	
		while ( (flag=ext_imapflags_iter_get_flag(&flit)) != NULL ) {		
			if (flag != NULL && *flag != '\\') {
				/* keyword */
				array_append(&keywords, &flag, 1);
			} else {
				/* system flag */
				if (flag == NULL || strcasecmp(flag, "\\flagged") == 0)
					ctx->flags |= MAIL_FLAGGED;
				else if (strcasecmp(flag, "\\answered") == 0)
					ctx->flags |= MAIL_ANSWERED;
				else if (strcasecmp(flag, "\\deleted") == 0)
					ctx->flags |= MAIL_DELETED;
				else if (strcasecmp(flag, "\\seen") == 0)
					ctx->flags |= MAIL_SEEN;
				else if (strcasecmp(flag, "\\draft") == 0)
					ctx->flags |= MAIL_DRAFT;
			}
		}
	}
	
	(void)array_append_space(&keywords);
	ctx->keywords = array_idx(&keywords, 0);
	*se_context = (void *) ctx;

	t_pop();
	
	return result;
}

static bool seff_flags_read_implicit_context
(const struct sieve_side_effect *seffect ATTR_UNUSED, 
	const struct sieve_runtime_env *renv, sieve_size_t *address ATTR_UNUSED,
	void **se_context)
{
	bool result = TRUE;
	pool_t pool = sieve_result_pool(renv->result);
	struct seff_flags_context *ctx;
	const char *flag;
	struct ext_imapflags_iter flit;
	ARRAY_DEFINE(keywords, const char *);
	
	ctx = p_new(pool, struct seff_flags_context, 1);
	p_array_init(&keywords, pool, 2);
	
	t_push();
		
	/* Unpack */
	ext_imapflags_get_flags_init(&flit, renv, NULL, 0);
	while ( (flag=ext_imapflags_iter_get_flag(&flit)) != NULL ) {		
		if (flag != NULL && *flag != '\\') {
			/* keyword */
			array_append(&keywords, &flag, 1);
		} else {
			/* system flag */
			if (flag == NULL || strcasecmp(flag, "\\flagged") == 0)
				ctx->flags |= MAIL_FLAGGED;
			else if (strcasecmp(flag, "\\answered") == 0)
				ctx->flags |= MAIL_ANSWERED;
			else if (strcasecmp(flag, "\\deleted") == 0)
				ctx->flags |= MAIL_DELETED;
			else if (strcasecmp(flag, "\\seen") == 0)
				ctx->flags |= MAIL_SEEN;
			else if (strcasecmp(flag, "\\draft") == 0)
				ctx->flags |= MAIL_DRAFT;
		}
	}
	
	(void)array_append_space(&keywords);
	ctx->keywords = array_idx(&keywords, 0);
	*se_context = (void *) ctx;

	t_pop();
	
	return result;
}


static void seff_flags_print
(const struct sieve_side_effect *seffect ATTR_UNUSED, 
	const struct sieve_action *action ATTR_UNUSED, 
	void *se_context ATTR_UNUSED, bool *keep)
{
	struct seff_flags_context *ctx = (struct seff_flags_context *) se_context;
	const char *const *keywords = ctx->keywords;
	
	printf("        + add flags:");

	if ( (ctx->flags & MAIL_FLAGGED) > 0 )
		printf(" \\flagged\n");

	if ( (ctx->flags & MAIL_ANSWERED) > 0 )
		printf(" \\answered");
		
	if ( (ctx->flags & MAIL_DELETED) > 0 )
		printf(" \\deleted");
					
	if ( (ctx->flags & MAIL_SEEN) > 0 )
		printf(" \\seen");
		
	if ( (ctx->flags & MAIL_DRAFT) > 0 )
		printf(" \\draft");

	while ( *keywords != NULL ) {
		printf(" %s", *keywords);
		
		keywords++;
	};
	
	printf("\n");

	*keep = TRUE;
}

static bool seff_flags_post_execute
(const struct sieve_side_effect *seffect ATTR_UNUSED, 
	const struct sieve_action *action ATTR_UNUSED, 
	const struct sieve_action_exec_env *aenv, 
	void *se_context, void *tr_context)
{	
	struct seff_flags_context *ctx = (struct seff_flags_context *) se_context;
	struct act_store_transaction *trans = 
		(struct act_store_transaction *) tr_context;
	struct mail_keywords *keywords;

	if ( trans->dest_mail == NULL ) return TRUE;

	if ( *(ctx->keywords) != NULL ) {
	 	if (mailbox_keywords_create(trans->box, ctx->keywords, &keywords) < 0) {
			sieve_result_error(aenv, "invalid keywords");
			return FALSE;
		}
		
		/* Update message keywords. */
		mail_update_keywords(trans->dest_mail, MODIFY_ADD, keywords);
	}

	/* Update message flags. */
	mail_update_flags(trans->dest_mail, MODIFY_ADD, ctx->flags);
	
	return TRUE;
}


