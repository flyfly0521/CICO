#pragma once

namespace neug::main { class ClientContext; }
namespace neug::binder {
class Binder;
class BoundStatement;
// Use a ready one-label COUNT index while retaining the original OPTIONAL
// expansion for every other label of an already bound union-label anchor.
void rewritePartialCounts(BoundStatement& statement,
                          main::ClientContext& context, Binder* binder);
}
