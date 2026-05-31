#include "interface_pointer.h"

template<typename InterfaceType>
InterfacePtr<InterfaceType>::InterfacePtr() :
    m_ptr(nullptr)
{
}

template<typename InterfaceType>
InterfacePtr<InterfaceType>::InterfacePtr(InterfaceType* ptr) :
    m_ptr(ptr)
{
}

template<typename InterfaceType>
InterfacePtr<InterfaceType>::InterfacePtr(InterfacePtr&& other) noexcept
    : m_ptr(other.m_ptr)
{
    other.m_ptr = nullptr;
}

template<typename InterfaceType>
InterfacePtr<InterfaceType>::~InterfacePtr()
{
    if (m_ptr)
    {
        m_ptr->Release();
        m_ptr = nullptr;
    }
}

template<typename InterfaceType>
InterfacePtr<InterfaceType>& InterfacePtr<InterfaceType>::operator=(InterfaceType* ptr)
{
    if (m_ptr != ptr && m_ptr != nullptr)
    {
        m_ptr->Release();
    }
    m_ptr = ptr;
    return *this;
}

template<typename InterfaceType>
InterfacePtr<InterfaceType>& InterfacePtr<InterfaceType>::operator=(InterfacePtr&& other) noexcept
{
    if (this != &other)
    {
        if (m_ptr) {
            m_ptr->Release();
        }
        m_ptr = other.m_ptr;
        other.m_ptr = nullptr;
    }
    return *this;
}

template<typename InterfaceType>
InterfaceType * InterfacePtr<InterfaceType>::Detach()
{
    InterfaceType * interface = m_ptr;
    m_ptr = nullptr;
    return interface;
}

template<typename InterfaceType>
InterfaceType ** InterfacePtr<InterfaceType>::GetAddressForSet()
{
    if (m_ptr)
    {
        m_ptr->Release();
        m_ptr = nullptr;
    }
    return &m_ptr;
}

template<typename InterfaceType>
InterfaceType * InterfacePtr<InterfaceType>::Get() const
{
    return m_ptr;
}

template<typename InterfaceType>
InterfaceType * InterfacePtr<InterfaceType>::operator->() const
{
    return m_ptr;
}

template<typename InterfaceType>
InterfaceType & InterfacePtr<InterfaceType>::operator*() const
{
    return *m_ptr;
}

template<typename InterfaceType>
InterfacePtr<InterfaceType>::operator bool() const
{
    return m_ptr != nullptr;
}

template<typename InterfaceType>
InterfacePtrShared<InterfaceType>::InterfacePtrShared() : 
    m_ptr(nullptr) 
{
}

template<typename InterfaceType>
InterfacePtrShared<InterfaceType>::InterfacePtrShared(InterfaceType* ptr) :
    m_ptr(ptr, [](InterfaceType* p) 
    {
        if (p)
        {
            p->Release();
        }
    }) 
{
}

template<typename InterfaceType>
InterfacePtrShared<InterfaceType> & InterfacePtrShared<InterfaceType>::operator=(InterfaceType * ptr)
{
    m_ptr = std::shared_ptr<InterfaceType>(ptr, [](InterfaceType* p) { if (p) p->Release(); });
    return *this;
}

template<typename InterfaceType>
InterfaceType * InterfacePtrShared<InterfaceType>::operator->() const
{
    return m_ptr.get();
}

template<typename InterfaceType>
InterfaceType & InterfacePtrShared<InterfaceType>::operator*() const {
    return *m_ptr;
}

template<typename InterfaceType>
InterfacePtrShared<InterfaceType>::operator bool() const {
    return static_cast<bool>(m_ptr);
}

template<typename InterfaceType>
InterfaceType * InterfacePtrShared<InterfaceType>::get() const {
    return m_ptr.get();
}