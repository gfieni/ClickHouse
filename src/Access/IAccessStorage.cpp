#include <Access/IAccessStorage.h>
#include <Access/Authentication.h>
#include <Access/Credentials.h>
#include <Access/User.h>
#include <Common/Exception.h>
#include <Common/quoteString.h>
#include <IO/WriteHelpers.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/Logger.h>
#include <base/FnTraits.h>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>


namespace DB
{
namespace ErrorCodes
{
    extern const int ACCESS_ENTITY_ALREADY_EXISTS;
    extern const int ACCESS_ENTITY_NOT_FOUND;
    extern const int ACCESS_STORAGE_READONLY;
    extern const int WRONG_PASSWORD;
    extern const int IP_ADDRESS_NOT_ALLOWED;
    extern const int AUTHENTICATION_FAILED;
    extern const int LOGICAL_ERROR;
}


namespace
{
    String outputID(const UUID & id)
    {
        return "ID(" + toString(id) + ")";
    }

    String formatTypeWithNameOrID(const IAccessStorage & storage, const UUID & id)
    {
        auto entity = storage.tryRead(id);
        if (entity)
            return entity->formatTypeWithName();
        return outputID(id);
    }


    template <typename Func>
    bool tryCall(const Func & function)
    {
        try
        {
            function();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }


    class ErrorsTracker
    {
    public:
        explicit ErrorsTracker(size_t count_) { succeed.reserve(count_); }

        template <typename Func>
        bool tryCall(const Func & func)
        {
            try
            {
                func();
            }
            catch (Exception & e)
            {
                if (!exception)
                    exception.emplace(e);
                succeed.push_back(false);
                return false;
            }
            catch (Poco::Exception & e)
            {
                if (!exception)
                    exception.emplace(Exception::CreateFromPocoTag{}, e);
                succeed.push_back(false);
                return false;
            }
            catch (std::exception & e)
            {
                if (!exception)
                    exception.emplace(Exception::CreateFromSTDTag{}, e);
                succeed.push_back(false);
                return false;
            }
            succeed.push_back(true);
            return true;
        }

        bool errors() const { return exception.has_value(); }

        void showErrors(const char * format, Fn<String(size_t)> auto && get_name_function)
        {
            if (!exception)
                return;

            Strings succeeded_names_list;
            Strings failed_names_list;
            for (size_t i = 0; i != succeed.size(); ++i)
            {
                String name = get_name_function(i);
                if (succeed[i])
                    succeeded_names_list.emplace_back(name);
                else
                    failed_names_list.emplace_back(name);
            }
            String succeeded_names = boost::algorithm::join(succeeded_names_list, ", ");
            String failed_names = boost::algorithm::join(failed_names_list, ", ");
            if (succeeded_names.empty())
                succeeded_names = "none";

            String error_message = format;
            boost::replace_all(error_message, "{succeeded_names}", succeeded_names);
            boost::replace_all(error_message, "{failed_names}", failed_names);
            exception->addMessage(error_message);
            exception->rethrow();
        }

    private:
        std::vector<bool> succeed;
        std::optional<Exception> exception;
    };
}


std::vector<UUID> IAccessStorage::findAll(AccessEntityType type) const
{
    return findAllImpl(type);
}


std::optional<UUID> IAccessStorage::find(AccessEntityType type, const String & name) const
{
    return findImpl(type, name);
}


std::vector<UUID> IAccessStorage::find(AccessEntityType type, const Strings & names) const
{
    std::vector<UUID> ids;
    ids.reserve(names.size());
    for (const String & name : names)
    {
        auto id = findImpl(type, name);
        if (id)
            ids.push_back(*id);
    }
    return ids;
}


UUID IAccessStorage::getID(AccessEntityType type, const String & name) const
{
    auto id = findImpl(type, name);
    if (id)
        return *id;
    throwNotFound(type, name);
}


std::vector<UUID> IAccessStorage::getIDs(AccessEntityType type, const Strings & names) const
{
    std::vector<UUID> ids;
    ids.reserve(names.size());
    for (const String & name : names)
        ids.push_back(getID(type, name));
    return ids;
}


String IAccessStorage::readName(const UUID & id) const
{
    return *readNameImpl(id, /* throw_if_not_exists = */ true);
}


std::optional<String> IAccessStorage::readName(const UUID & id, bool throw_if_not_exists) const
{
    return readNameImpl(id, throw_if_not_exists);
}


Strings IAccessStorage::readNames(const std::vector<UUID> & ids, bool throw_if_not_exists) const
{
    Strings res;
    res.reserve(ids.size());
    for (const auto & id : ids)
    {
        if (auto name = readNameImpl(id, throw_if_not_exists))
            res.emplace_back(std::move(name).value());
    }
    return res;
}


std::optional<String> IAccessStorage::tryReadName(const UUID & id) const
{
    return readName(id, /* throw_if_not_exists = */ false);
}


Strings IAccessStorage::tryReadNames(const std::vector<UUID> & ids) const
{
    return readNames(ids, /* throw_if_not_exists = */ false);
}


std::optional<String> IAccessStorage::readNameImpl(const UUID & id, bool throw_if_not_exists) const
{
    if (auto entity = read(id, throw_if_not_exists))
        return entity->getName();
    return std::nullopt;
}


UUID IAccessStorage::insert(const AccessEntityPtr & entity)
{
    return *insertImpl(entity, /* replace_if_exists = */ false, /* throw_if_exists = */ true);
}


std::optional<UUID> IAccessStorage::insert(const AccessEntityPtr & entity, bool replace_if_exists, bool throw_if_exists)
{
    return insertImpl(entity, replace_if_exists, throw_if_exists);
}


std::vector<UUID> IAccessStorage::insert(const std::vector<AccessEntityPtr> & multiple_entities, bool replace_if_exists, bool throw_if_exists)
{
    if (multiple_entities.empty())
        return {};

    if (multiple_entities.size() == 1)
    {
        if (auto id = insert(multiple_entities[0], replace_if_exists, throw_if_exists))
            return {*id};
        return {};
    }

    std::vector<AccessEntityPtr> successfully_inserted;
    try
    {
        std::vector<UUID> ids;
        for (const auto & entity : multiple_entities)
        {
            if (auto id = insertImpl(entity, replace_if_exists, throw_if_exists))
            {
                successfully_inserted.push_back(entity);
                ids.push_back(*id);
            }
        }
        return ids;
    }
    catch (Exception & e)
    {
        if (!successfully_inserted.empty())
        {
            String successfully_inserted_str;
            for (auto entity : successfully_inserted)
            {
                if (!successfully_inserted_str.empty())
                    successfully_inserted_str += ", ";
                successfully_inserted_str += entity->formatTypeWithName();
            }
            e.addMessage("After successfully inserting {}/{}: {}", successfully_inserted.size(), multiple_entities.size(), successfully_inserted_str);
        }
        e.rethrow();
        __builtin_unreachable();
    }
}


std::optional<UUID> IAccessStorage::tryInsert(const AccessEntityPtr & entity)
{
    return insert(entity, /* replace_if_exists = */ false, /* throw_if_exists = */ false);
}


std::vector<UUID> IAccessStorage::tryInsert(const std::vector<AccessEntityPtr> & multiple_entities)
{
    return insert(multiple_entities, /* replace_if_exists = */ false, /* throw_if_exists = */ false);
}


UUID IAccessStorage::insertOrReplace(const AccessEntityPtr & entity)
{
    return *insertImpl(entity, /* replace_if_exists = */ true, /* throw_if_exists = */ false);
}


std::vector<UUID> IAccessStorage::insertOrReplace(const std::vector<AccessEntityPtr> & multiple_entities)
{
    return insert(multiple_entities, /* replace_if_exists = */ true, /* throw_if_exists = */ false);
}


void IAccessStorage::remove(const UUID & id)
{
    removeImpl(id);
}


void IAccessStorage::remove(const std::vector<UUID> & ids)
{
    ErrorsTracker tracker(ids.size());

    for (const auto & id : ids)
    {
        auto func = [&] { removeImpl(id); };
        tracker.tryCall(func);
    }

    if (tracker.errors())
    {
        auto get_name_function = [&](size_t i) { return formatTypeWithNameOrID(*this, ids[i]); };
        tracker.showErrors("Couldn't remove {failed_names}. Successfully removed: {succeeded_names}", get_name_function);
    }
}


bool IAccessStorage::tryRemove(const UUID & id)
{
    auto func = [&] { removeImpl(id); };
    return tryCall(func);
}


std::vector<UUID> IAccessStorage::tryRemove(const std::vector<UUID> & ids)
{
    std::vector<UUID> removed_ids;
    for (const auto & id : ids)
    {
        auto func = [&] { removeImpl(id); };
        if (tryCall(func))
            removed_ids.push_back(id);
    }
    return removed_ids;
}


void IAccessStorage::update(const UUID & id, const UpdateFunc & update_func)
{
    updateImpl(id, update_func);
}


void IAccessStorage::update(const std::vector<UUID> & ids, const UpdateFunc & update_func)
{
    ErrorsTracker tracker(ids.size());

    for (const auto & id : ids)
    {
        auto func = [&] { updateImpl(id, update_func); };
        tracker.tryCall(func);
    }

    if (tracker.errors())
    {
        auto get_name_function = [&](size_t i) { return formatTypeWithNameOrID(*this, ids[i]); };
        tracker.showErrors("Couldn't update {failed_names}. Successfully updated: {succeeded_names}", get_name_function);
    }
}


bool IAccessStorage::tryUpdate(const UUID & id, const UpdateFunc & update_func)
{
    auto func = [&] { updateImpl(id, update_func); };
    return tryCall(func);
}


std::vector<UUID> IAccessStorage::tryUpdate(const std::vector<UUID> & ids, const UpdateFunc & update_func)
{
    std::vector<UUID> updated_ids;
    for (const auto & id : ids)
    {
        auto func = [&] { updateImpl(id, update_func); };
        if (tryCall(func))
            updated_ids.push_back(id);
    }
    return updated_ids;
}


scope_guard IAccessStorage::subscribeForChanges(AccessEntityType type, const OnChangedHandler & handler) const
{
    return subscribeForChangesImpl(type, handler);
}


scope_guard IAccessStorage::subscribeForChanges(const UUID & id, const OnChangedHandler & handler) const
{
    return subscribeForChangesImpl(id, handler);
}


scope_guard IAccessStorage::subscribeForChanges(const std::vector<UUID> & ids, const OnChangedHandler & handler) const
{
    scope_guard subscriptions;
    for (const auto & id : ids)
        subscriptions.join(subscribeForChangesImpl(id, handler));
    return subscriptions;
}


void IAccessStorage::notify(const Notifications & notifications)
{
    for (const auto & [fn, id, new_entity] : notifications)
        fn(id, new_entity);
}


UUID IAccessStorage::authenticate(
    const Credentials & credentials,
    const Poco::Net::IPAddress & address,
    const ExternalAuthenticators & external_authenticators,
    bool replace_exception_with_cannot_authenticate) const
{
    try
    {
        return authenticateImpl(credentials, address, external_authenticators);
    }
    catch (...)
    {
        if (!replace_exception_with_cannot_authenticate)
            throw;

        tryLogCurrentException(getLogger(), "from: " + address.toString() + ", user: " + credentials.getUserName()  + ": Authentication failed");
        throwCannotAuthenticate(credentials.getUserName());
    }
}


UUID IAccessStorage::authenticateImpl(
    const Credentials & credentials,
    const Poco::Net::IPAddress & address,
    const ExternalAuthenticators & external_authenticators) const
{
    if (auto id = find<User>(credentials.getUserName()))
    {
        if (auto user = tryRead<User>(*id))
        {
            if (!isAddressAllowed(*user, address))
                throwAddressNotAllowed(address);

            if (!areCredentialsValid(*user, credentials, external_authenticators))
                throwInvalidCredentials();

            return *id;
        }
    }
    throwNotFound(AccessEntityType::USER, credentials.getUserName());
}


bool IAccessStorage::areCredentialsValid(
    const User & user,
    const Credentials & credentials,
    const ExternalAuthenticators & external_authenticators) const
{
    if (!credentials.isReady())
        return false;

    if (credentials.getUserName() != user.getName())
        return false;

    return Authentication::areCredentialsValid(credentials, user.auth_data, external_authenticators);
}


bool IAccessStorage::isAddressAllowed(const User & user, const Poco::Net::IPAddress & address) const
{
    return user.allowed_client_hosts.contains(address);
}


UUID IAccessStorage::generateRandomID()
{
    static Poco::UUIDGenerator generator;
    UUID id;
    generator.createRandom().copyTo(reinterpret_cast<char *>(&id));
    return id;
}


Poco::Logger * IAccessStorage::getLogger() const
{
    Poco::Logger * ptr = log.load();
    if (!ptr)
        log.store(ptr = &Poco::Logger::get("Access(" + storage_name + ")"), std::memory_order_relaxed);
    return ptr;
}


void IAccessStorage::throwNotFound(const UUID & id) const
{
    throw Exception(outputID(id) + " not found in " + getStorageName(), ErrorCodes::ACCESS_ENTITY_NOT_FOUND);
}


void IAccessStorage::throwNotFound(AccessEntityType type, const String & name) const
{
    int error_code = AccessEntityTypeInfo::get(type).not_found_error_code;
    throw Exception("There is no " + formatEntityTypeWithName(type, name) + " in " + getStorageName(), error_code);
}


void IAccessStorage::throwBadCast(const UUID & id, AccessEntityType type, const String & name, AccessEntityType required_type)
{
    throw Exception(
        outputID(id) + ": " + formatEntityTypeWithName(type, name) + " expected to be of type " + toString(required_type),
        ErrorCodes::LOGICAL_ERROR);
}


void IAccessStorage::throwIDCollisionCannotInsert(const UUID & id, AccessEntityType type, const String & name, AccessEntityType existing_type, const String & existing_name) const
{
    throw Exception(
        formatEntityTypeWithName(type, name) + ": cannot insert because the " + outputID(id) + " is already used by "
            + formatEntityTypeWithName(existing_type, existing_name) + " in " + getStorageName(),
        ErrorCodes::ACCESS_ENTITY_ALREADY_EXISTS);
}


void IAccessStorage::throwNameCollisionCannotInsert(AccessEntityType type, const String & name) const
{
    throw Exception(
        formatEntityTypeWithName(type, name) + ": cannot insert because " + formatEntityTypeWithName(type, name) + " already exists in "
            + getStorageName(),
        ErrorCodes::ACCESS_ENTITY_ALREADY_EXISTS);
}


void IAccessStorage::throwNameCollisionCannotRename(AccessEntityType type, const String & old_name, const String & new_name) const
{
    throw Exception(
        formatEntityTypeWithName(type, old_name) + ": cannot rename to " + backQuote(new_name) + " because "
            + formatEntityTypeWithName(type, new_name) + " already exists in " + getStorageName(),
        ErrorCodes::ACCESS_ENTITY_ALREADY_EXISTS);
}


void IAccessStorage::throwReadonlyCannotInsert(AccessEntityType type, const String & name) const
{
    throw Exception(
        "Cannot insert " + formatEntityTypeWithName(type, name) + " to " + getStorageName() + " because this storage is readonly",
        ErrorCodes::ACCESS_STORAGE_READONLY);
}


void IAccessStorage::throwReadonlyCannotUpdate(AccessEntityType type, const String & name) const
{
    throw Exception(
        "Cannot update " + formatEntityTypeWithName(type, name) + " in " + getStorageName() + " because this storage is readonly",
        ErrorCodes::ACCESS_STORAGE_READONLY);
}


void IAccessStorage::throwReadonlyCannotRemove(AccessEntityType type, const String & name) const
{
    throw Exception(
        "Cannot remove " + formatEntityTypeWithName(type, name) + " from " + getStorageName() + " because this storage is readonly",
        ErrorCodes::ACCESS_STORAGE_READONLY);
}

void IAccessStorage::throwAddressNotAllowed(const Poco::Net::IPAddress & address)
{
    throw Exception("Connections from " + address.toString() + " are not allowed", ErrorCodes::IP_ADDRESS_NOT_ALLOWED);
}

void IAccessStorage::throwInvalidCredentials()
{
    throw Exception("Invalid credentials", ErrorCodes::WRONG_PASSWORD);
}

void IAccessStorage::throwCannotAuthenticate(const String & user_name)
{
    /// We use the same message for all authentication failures because we don't want to give away any unnecessary information for security reasons,
    /// only the log will show the exact reason.
    throw Exception(user_name + ": Authentication failed: password is incorrect or there is no user with such name", ErrorCodes::AUTHENTICATION_FAILED);
}

}
