/* 
** No copyright?!
**
** $FreeBSD: projects/doscmd/callback.h,v 1.3 2001/08/08 10:58:50 tg Exp $
*/
typedef void		(*callback_t)(regcontext_t *);

void		register_callback(u_int32_t, callback_t, const char *);
callback_t	find_callback(u_int32_t);
u_int32_t		insert_generic_trampoline(size_t, u_char *);
u_int32_t		insert_softint_trampoline(void);
u_int32_t		insert_fossil_softint_trampoline(void);
u_int32_t		insert_hardint_trampoline(void);
u_int32_t		insert_null_trampoline(void);
