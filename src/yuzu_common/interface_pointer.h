#pragma once
#include <memory>

template<typename InterfaceType>
class InterfacePtr
{
public:
    InterfacePtr();
    InterfacePtr(InterfaceType*);
    InterfacePtr(InterfacePtr&& other) noexcept;
    ~InterfacePtr();

    InterfacePtr & operator=(InterfaceType * ptr);
    InterfacePtr & operator=(InterfacePtr && other) noexcept;
    InterfaceType * Detach();
    InterfaceType ** GetAddressForSet();
    InterfaceType * Get() const;

    InterfaceType * operator->() const;
    InterfaceType & operator*() const;
    operator bool() const;

protected:
    InterfacePtr(const InterfacePtr &) = delete;
    InterfacePtr & operator=(const InterfacePtr &) = delete;

    InterfaceType * m_ptr;
};

template<typename InterfaceType>
class InterfacePtrShared
{
public:
    InterfacePtrShared();
    InterfacePtrShared(InterfaceType * ptr);

    InterfacePtrShared(const InterfacePtrShared & other) = default;
    InterfacePtrShared(InterfacePtrShared && other) noexcept = default;

    ~InterfacePtrShared() = default;

    InterfacePtrShared & operator=(InterfaceType * ptr); 
    InterfacePtrShared & operator=(const InterfacePtrShared & other) = default;
    InterfacePtrShared & operator=(InterfacePtrShared && other) noexcept = default;

    InterfaceType * operator->() const;
    InterfaceType & operator*() const;
    operator bool() const;

    InterfaceType * get() const;

private:
    std::shared_ptr<InterfaceType> m_ptr;
};