#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType kMethodType = "$ccls/memberHierarchy";

struct In_CclsMemberHierarchy : public RequestInMessage {
  MethodType GetMethodType() const override { return kMethodType; }

  struct Params {
    // If id is specified, expand a node; otherwise textDocument+position should
    // be specified for building the root and |levels| of nodes below.
    lsTextDocumentIdentifier textDocument;
    lsPosition position;

    Maybe<QueryTypeId> id;

    bool qualified = false;
    int levels = 1;
  };
  Params params;
};

MAKE_REFLECT_STRUCT(In_CclsMemberHierarchy::Params,
                    textDocument,
                    position,
                    id,
                    qualified,
                    levels);
MAKE_REFLECT_STRUCT(In_CclsMemberHierarchy, id, params);
REGISTER_IN_MESSAGE(In_CclsMemberHierarchy);

struct Out_CclsMemberHierarchy
    : public lsOutMessage<Out_CclsMemberHierarchy> {
  struct Entry {
    QueryTypeId id;
    std::string_view name;
    std::string fieldName;
    lsLocation location;
    // For unexpanded nodes, this is an upper bound because some entities may be
    // undefined. If it is 0, there are no members.
    int numChildren = 0;
    // Empty if the |levels| limit is reached.
    std::vector<Entry> children;
  };
  lsRequestId id;
  std::optional<Entry> result;
};
MAKE_REFLECT_STRUCT(Out_CclsMemberHierarchy::Entry,
                    id,
                    name,
                    fieldName,
                    location,
                    numChildren,
                    children);
MAKE_REFLECT_STRUCT(Out_CclsMemberHierarchy, jsonrpc, id, result);

bool Expand(MessageHandler* m,
            Out_CclsMemberHierarchy::Entry* entry,
            bool qualified,
            int levels);

// Add a field to |entry| which is a Func/Type.
void DoField(MessageHandler* m,
             Out_CclsMemberHierarchy::Entry* entry,
             const QueryVar& var,
             bool qualified,
             int levels) {
  const QueryVar::Def* def1 = var.AnyDef();
  if (!def1)
    return;
  Out_CclsMemberHierarchy::Entry entry1;
  if (qualified)
    entry1.fieldName = def1->detailed_name;
  else
    entry1.fieldName = def1->detailed_name.substr(0, def1->qual_name_offset) +
                       std::string(def1->Name(false));
  if (def1->spell) {
    if (std::optional<lsLocation> loc =
            GetLsLocation(m->db, m->working_files, *def1->spell))
      entry1.location = *loc;
  }
  if (def1->type) {
    entry1.id = *def1->type;
    if (Expand(m, &entry1, qualified, levels))
      entry->children.push_back(std::move(entry1));
  } else {
    entry1.id = QueryTypeId();
    entry->children.push_back(std::move(entry1));
  }
}

// Expand a type node by adding members recursively to it.
bool Expand(MessageHandler* m,
            Out_CclsMemberHierarchy::Entry* entry,
            bool qualified,
            int levels) {
  const QueryType& type = m->db->types[entry->id.id];
  const QueryType::Def* def = type.AnyDef();
  // builtin types have no declaration and empty |qualified|.
  if (CXType_FirstBuiltin <= type.usr && type.usr <= CXType_LastBuiltin) {
    entry->name = ClangBuiltinTypeName(CXTypeKind(type.usr));
    return true;
  }
  if (!def)
    return false;
  entry->name = def->Name(qualified);
  std::unordered_set<Usr> seen;
  if (levels > 0) {
    std::vector<const QueryType*> stack;
    seen.insert(type.usr);
    stack.push_back(&type);
    while (stack.size()) {
      const auto* def = stack.back()->AnyDef();
      stack.pop_back();
      if (def) {
        EachDefinedEntity(m->db->types, def->bases, [&](QueryType& type1) {
          if (!seen.count(type1.usr)) {
            seen.insert(type1.usr);
            stack.push_back(&type1);
          }
        });
        if (def->alias_of) {
          const QueryType::Def* def1 = m->db->types[def->alias_of->id].AnyDef();
          Out_CclsMemberHierarchy::Entry entry1;
          entry1.id = *def->alias_of;
          if (def1 && def1->spell) {
            // The declaration of target type.
            if (std::optional<lsLocation> loc =
                    GetLsLocation(m->db, m->working_files, *def1->spell))
              entry1.location = *loc;
          } else if (def->spell) {
            // Builtin types have no declaration but the typedef declaration
            // itself is useful.
            if (std::optional<lsLocation> loc =
                    GetLsLocation(m->db, m->working_files, *def->spell))
              entry1.location = *loc;
          }
          if (def1 && qualified)
            entry1.fieldName = def1->detailed_name;
          if (Expand(m, &entry1, qualified, levels - 1)) {
            // For builtin types |name| is set.
            if (entry1.fieldName.empty())
              entry1.fieldName = std::string(entry1.name);
            entry->children.push_back(std::move(entry1));
          }
        } else {
          EachDefinedEntity(m->db->vars, def->vars, [&](QueryVar& var) {
            DoField(m, entry, var, qualified, levels - 1);
          });
        }
      }
    }
    entry->numChildren = int(entry->children.size());
  } else
    entry->numChildren = def->alias_of ? 1 : int(def->vars.size());
  return true;
}

struct Handler_CclsMemberHierarchy
    : BaseMessageHandler<In_CclsMemberHierarchy> {
  MethodType GetMethodType() const override { return kMethodType; }

  std::optional<Out_CclsMemberHierarchy::Entry> BuildInitial(QueryFuncId root_id,
                                                          bool qualified,
                                                          int levels) {
    const auto* def = db->funcs[root_id.id].AnyDef();
    if (!def)
      return {};

    Out_CclsMemberHierarchy::Entry entry;
    // Not type, |id| is invalid.
    entry.name = std::string(def->Name(qualified));
    if (def->spell) {
      if (std::optional<lsLocation> loc =
              GetLsLocation(db, working_files, *def->spell))
        entry.location = *loc;
    }
    EachDefinedEntity(db->vars, def->vars, [&](QueryVar& var) {
      DoField(this, &entry, var, qualified, levels - 1);
    });
    return entry;
  }

  std::optional<Out_CclsMemberHierarchy::Entry> BuildInitial(QueryTypeId root_id,
                                                          bool qualified,
                                                          int levels) {
    const auto* def = db->types[root_id.id].AnyDef();
    if (!def)
      return {};

    Out_CclsMemberHierarchy::Entry entry;
    entry.id = root_id;
    if (def->spell) {
      if (std::optional<lsLocation> loc =
              GetLsLocation(db, working_files, *def->spell))
        entry.location = *loc;
    }
    Expand(this, &entry, qualified, levels);
    return entry;
  }

  void Run(In_CclsMemberHierarchy* request) override {
    const auto& params = request->params;
    Out_CclsMemberHierarchy out;
    out.id = request->id;

    if (params.id) {
      Out_CclsMemberHierarchy::Entry entry;
      entry.id = *request->params.id;
      // entry.name is empty as it is known by the client.
      if (entry.id.id < db->types.size() &&
          Expand(this, &entry, params.qualified, params.levels))
        out.result = std::move(entry);
    } else {
      QueryFile* file;
      if (!FindFileOrFail(db, project, request->id,
                          params.textDocument.uri.GetPath(), &file))
        return;
      WorkingFile* working_file =
          working_files->GetFileByFilename(file->def->path);
      for (SymbolRef sym :
           FindSymbolsAtLocation(working_file, file, params.position)) {
        switch (sym.kind) {
          case SymbolKind::Func:
            out.result = BuildInitial(QueryFuncId(sym.id), params.qualified,
                                      params.levels);
            break;
          case SymbolKind::Type:
            out.result = BuildInitial(QueryTypeId(sym.id), params.qualified,
                                      params.levels);
            break;
          case SymbolKind::Var: {
            const QueryVar::Def* def = db->GetVar(sym).AnyDef();
            if (def && def->type)
              out.result = BuildInitial(QueryTypeId(*def->type),
                                        params.qualified, params.levels);
            break;
          }
          default:
            continue;
        }
        break;
      }
    }

    QueueManager::WriteStdout(kMethodType, out);
  }
};
REGISTER_MESSAGE_HANDLER(Handler_CclsMemberHierarchy);

}  // namespace