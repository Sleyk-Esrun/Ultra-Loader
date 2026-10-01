/* SPDX-License-Identifier: MIT
 * stubs_cruisn_noop.c -- System N2 has no Cruis'n Blast title to stub.
 *
 * stubs_glibc.c unconditionally calls cruisn_stubs_register()/_late() (a
 * Raw-Thrills-g5/Cruis'n-Blast-specific symbol registration step) -- honest
 * no-ops here since no N2 title needs it. See modules/rawthrills_g5/src/
 * runtime/stubs_cruisn.c for the real implementation, used only where it
 * actually applies.
 */
struct LoaderCtx;
void cruisn_stubs_register(struct LoaderCtx *ctx) { (void)ctx; }
void cruisn_stubs_register_late(struct LoaderCtx *ctx) { (void)ctx; }
