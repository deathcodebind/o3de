/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/DOM/Backends/JSON/JsonSerializationUtils.h>
#include <AzCore/DOM/DomPrefixTree.h>
#include <AzCore/DOM/DomUtils.h>
#include <AzCore/Serialization/PointerObject.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/std/ranges/ranges_algorithm.h>
#include <AzFramework/DocumentPropertyEditor/ExpanderSettings.h>
#include <AzFramework/DocumentPropertyEditor/PropertyEditorNodes.h>
#include <AzFramework/DocumentPropertyEditor/Reflection/LegacyReflectionBridge.h>
#include <AzFramework/DocumentPropertyEditor/ReflectionAdapter.h>

namespace AZ::DocumentPropertyEditor
{
    struct ReflectionAdapterReflectionImpl : public AZ::Reflection::IReadWrite
    {
        AZ::SerializeContext* m_serializeContext;
        ReflectionAdapter* m_adapter;
        AdapterBuilder m_builder;
        // Look-up table of onChanged callbacks for handling property changes
        using OnChangedCallbackPrefixTree = AZ::Dom::DomPrefixTree<AZStd::function<Dom::Value(const Dom::Value&)>>;
        OnChangedCallbackPrefixTree m_onChangedCallbacks;

        //! This represents a container or associative container instance and has methods
        //! for interacting with the container.
        struct BoundContainer
        {
            // For constructing non-nested containers
            BoundContainer(AZ::SerializeContext::IDataContainer* container, void* containerInstance)
                : m_container(container)
                , m_containerInstance(containerInstance)
            {
            }

            static AZStd::unique_ptr<BoundContainer> CreateBoundContainer(
                void* instance, // This instance might be a container, a nested container element, or a non-container element
                const Reflection::IAttributes& attributes)
            {
                AZ_Assert(instance != nullptr, "Instance was nullptr when attempting to create a BoundContainer");

                AZ::Serialize::IDataContainer* container{};
                if (auto containerValue = attributes.Find(AZ::Reflection::DescriptorAttributes::Container);
                    containerValue && !containerValue->IsNull())
                {
                    if (auto containerObject = AZ::Dom::Utils::ValueToType<AZ::PointerObject>(*containerValue);
                        containerObject && containerObject->m_typeId == azrtti_typeid<AZ::Serialize::IDataContainer>())
                    {
                        container = reinterpret_cast<AZ::Serialize::IDataContainer*>(containerObject->m_address);
                    }
                }
                if (container != nullptr)
                {
                    return AZStd::make_unique<BoundContainer>(container, instance);
                }
                return nullptr;
            }

            Dom::Value GetContainerNode(ReflectionAdapterReflectionImpl* impl, const AZ::Dom::Path& path)
            {
                Dom::Value containerRow;
                const auto& findContainerProcedure = [&](const AZ::Dom::Path& nodePath, const ContainerEntry& containerEntry)
                {
                    if (containerRow.IsNull() && containerEntry.m_container && containerEntry.m_container->m_container == m_container)
                    {
                        containerRow = impl->m_adapter->GetContents()[nodePath];
                        return false;
                    }
                    return true;
                };

                // Find the row that contains the PropertyEditor for our actual container (if it exists)
                auto visitorFlags =
                    Dom::PrefixTreeTraversalFlags::ExcludeChildPaths | Dom::PrefixTreeTraversalFlags::TraverseMostToLeastSpecific;
                impl->m_containers.VisitPath(path, findContainerProcedure, visitorFlags);

                if (containerRow.IsNode())
                {
                    // Look within the Row for a PropertyEditor that has a SerializedPath field.
                    // This will be the container's editor w/ attributes.
                    for (auto it = containerRow.ArrayBegin(); it != containerRow.ArrayEnd(); ++it)
                    {
                        if (it->IsNode() && it->GetNodeName() == GetNodeName<Nodes::PropertyEditor>())
                        {
                            auto serializedFieldIt = it->FindMember(Reflection::DescriptorAttributes::SerializedPath);
                            if (serializedFieldIt != it->MemberEnd())
                            {
                                return *it;
                            }
                        }
                    }
                }
                return {};
            }

            void OnClear(ReflectionAdapterReflectionImpl* impl, const AZ::Dom::Path& path)
            {
                m_container->ClearElements(m_containerInstance, impl->m_serializeContext);

                auto containerNode = GetContainerNode(impl, path);
                Nodes::PropertyEditor::ChangeNotify.InvokeOnDomNode(containerNode);
                impl->m_adapter->NotifyResetDocument();
            }

            void OnAddElement(ReflectionAdapterReflectionImpl* impl, const AZ::Dom::Path& path)
            {
                if (m_container->IsFixedCapacity() && m_container->Size(m_containerInstance) >= m_container->Capacity(m_containerInstance))
                {
                    return;
                }

                const AZ::SerializeContext::ClassElement* containerClassElement =
                    m_container->GetElement(m_container->GetDefaultElementNameCrc());

                // The reserved element is an allocated instance of the IDataContainer's ValueType.
                // In an associative container, this would be a pair.
                m_reservedElementInstance = m_container->ReserveElement(m_containerInstance, containerClassElement);

                auto associativeContainer = m_container->GetAssociativeContainerInterface();
                if (associativeContainer)
                {
                    auto keyTypeAttribute = containerClassElement->FindAttribute(AZ_CRC_CE("KeyType"));
                    if (keyTypeAttribute)
                    {
                        // Get the key type and send it with the dataAddress in the message, then skip the store
                        // element below until we get an AddContainerKey message back from the DPE UI
                        auto* keyTypeData = azdynamic_cast<const AZ::Edit::AttributeData<AZ::Uuid>*>(keyTypeAttribute);
                        if (keyTypeData)
                        {
                            const AZ::TypeId& keyType = keyTypeData->Get(nullptr);
                            DocumentAdapterPtr reflectionAdapter =
                                AZStd::make_shared<ReflectionAdapter>(m_reservedElementInstance, keyType);
                            Nodes::Adapter::QueryKey.InvokeOnDomNode(impl->m_adapter->GetContents(), &reflectionAdapter, path);
                            return; // key queried; don't store the actual entry until the DPE handles the QueryKey message
                        }
                    }
                }

                m_container->StoreElement(m_containerInstance, m_reservedElementInstance);
                m_reservedElementInstance = nullptr;

                auto containerNode = GetContainerNode(impl, path);
                Nodes::PropertyEditor::ChangeNotify.InvokeOnDomNode(containerNode);
                impl->m_adapter->NotifyResetDocument();
            }

            void OnAddElementToAssociativeContainer(
                ReflectionAdapterReflectionImpl* impl,
                AZ::DocumentPropertyEditor::DocumentAdapterPtr* adapterContainingKey,
                const AZ::Dom::Path& containerPath)
            {
                AZ_Assert(m_reservedElementInstance != nullptr, "This BoundContainer has no reserved element to store");

                ReflectionAdapter* adapter = static_cast<ReflectionAdapter*>(adapterContainingKey->get());
                void* keyInstance = adapter->GetInstance();

                auto* associativeContainer = m_container->GetAssociativeContainerInterface();
                if (associativeContainer)
                {
                    associativeContainer->SetElementKey(m_reservedElementInstance, keyInstance);
                }

                m_container->StoreElement(m_containerInstance, m_reservedElementInstance);
                m_reservedElementInstance = nullptr;

                auto containerNode = GetContainerNode(impl, containerPath);
                Nodes::PropertyEditor::ChangeNotify.InvokeOnDomNode(containerNode);
                impl->m_adapter->NotifyResetDocument();
            };

            void RejectAssociativeContainerKey(ReflectionAdapterReflectionImpl* impl)
            {
                AZ_Assert(m_reservedElementInstance != nullptr, "This BoundContainer has no reserved element to free");
                m_container->FreeReservedElement(m_containerInstance, m_reservedElementInstance, impl->m_serializeContext);
                m_reservedElementInstance = nullptr;
            };

            AZ::SerializeContext::IDataContainer* m_container = nullptr;
            void* m_containerInstance = nullptr;

            // An element instance reserved through the IDataContainer API
            void* m_reservedElementInstance = nullptr;
        };

        //! This represents an element instance of a container or associative container with methods
        //! for interacting with that parent container. The element instance could be a container within
        //! another container or a non-container element.
        struct ContainerElement
        {
            // For constructing non-container elements
            ContainerElement(AZ::SerializeContext::IDataContainer* container, void* containerInstance, void* elementInstance)
                : m_container(container)
                , m_containerInstance(containerInstance)
                , m_elementInstance(elementInstance)
            {
            }

            static AZStd::unique_ptr<ContainerElement> CreateContainerElement(void* instance, const Reflection::IAttributes& attributes)
            {
                AZ_Assert(instance != nullptr, "Instance was nullptr when attempting to create a ContainerElement");

                AZ::Serialize::IDataContainer* parentContainer{};
                if (auto parentContainerValue = attributes.Find(AZ::Reflection::DescriptorAttributes::ParentContainer);
                    parentContainerValue && !parentContainerValue->IsNull())
                {
                    auto parentContainerObject = AZ::Dom::Utils::ValueToType<AZ::PointerObject>(*parentContainerValue);
                    if (parentContainerObject && parentContainerObject->m_typeId == azrtti_typeid<AZ::Serialize::IDataContainer>())
                    {
                        parentContainer = reinterpret_cast<AZ::Serialize::IDataContainer*>(parentContainerObject->m_address);
                    }
                }
                if (parentContainer != nullptr)
                {
                    auto parentContainerInstanceValue = attributes.Find(AZ::Reflection::DescriptorAttributes::ParentContainerInstance);
                    void* parentContainerInstance{};
                    AZStd::optional<AZ::PointerObject> parentContainerInstanceObject = AZ::Dom::Utils::ValueToType<AZ::PointerObject>(*parentContainerInstanceValue);
                    if (parentContainerInstanceObject.has_value() && parentContainerInstanceObject->IsValid())
                    {
                        parentContainerInstance = parentContainerInstanceObject->m_address;
                    }

                    // Check if this element is actually standing in for a direct child of a container. This is used in scenarios like
                    // maps, where the direct children are actually pairs of key/value, but we need to only show the value as an
                    // editable item who pretends that they can be removed directly from the container
                    auto containerElementOverrideValue = attributes.Find(AZ::Reflection::DescriptorAttributes::ContainerElementOverride);
                    if (containerElementOverrideValue)
                    {
                        AZStd::optional<AZ::PointerObject> containerElementOverrideObject = AZ::Dom::Utils::ValueToType<AZ::PointerObject>(*containerElementOverrideValue);
                        if (containerElementOverrideObject.has_value() && containerElementOverrideObject->IsValid())
                        {
                            instance = containerElementOverrideObject->m_address;
                        }
                    }

                    return AZStd::make_unique<ContainerElement>(parentContainer, parentContainerInstance, instance);
                }

                return nullptr;
            }

            Dom::Value GetContainerNode(ReflectionAdapterReflectionImpl* impl, const AZ::Dom::Path& path)
            {
                Dom::Value containerRow;
                const auto& findContainerProcedure = [&](const AZ::Dom::Path& nodePath, const ContainerEntry& containerEntry)
                {
                    if (containerRow.IsNull() && containerEntry.m_container && containerEntry.m_container->m_container == m_container)
                    {
                        containerRow = impl->m_adapter->GetContents()[nodePath];
                        return false; // We've found our container row, so stop the visitor
                    }
                    return true;
                };

                // Find the row that contains the PropertyEditor for our actual container (if it exists)
                auto visitorFlags =
                    Dom::PrefixTreeTraversalFlags::ExcludeChildPaths | Dom::PrefixTreeTraversalFlags::TraverseMostToLeastSpecific;
                impl->m_containers.VisitPath(path, findContainerProcedure, visitorFlags);

                if (containerRow.IsNode())
                {
                    // Look within the Row for a PropertyEditor that has a SerializedPath field.
                    // This will be the container's editor w/ attributes.
                    for (auto it = containerRow.ArrayBegin(); it != containerRow.ArrayEnd(); ++it)
                    {
                        if (it->IsNode() && it->GetNodeName() == GetNodeName<Nodes::PropertyEditor>())
                        {
                            auto serializedFieldIt = it->FindMember(Reflection::DescriptorAttributes::SerializedPath);
                            if (serializedFieldIt != it->MemberEnd())
                            {
                                return *it;
                            }
                        }
                    }
                }
                return {};
            }

            void OnRemoveElement(ReflectionAdapterReflectionImpl* impl, const AZ::Dom::Path& path)
            {
                m_container->RemoveElement(m_containerInstance, m_elementInstance, impl->m_serializeContext);
                auto containerNode = GetContainerNode(impl, path);
                Nodes::PropertyEditor::ChangeNotify.InvokeOnDomNode(containerNode);
                impl->m_adapter->NotifyResetDocument();
            }

            void OnMoveElement(ReflectionAdapterReflectionImpl* impl, const AZ::Dom::Path& path, AZ::s64 containerIndex, bool moveForward)
            {
                m_container->SwapElements(m_containerInstance, containerIndex, (moveForward ? containerIndex + 1 : containerIndex - 1));
                auto containerNode = GetContainerNode(impl, path);
                Nodes::PropertyEditor::ChangeNotify.InvokeOnDomNode(containerNode);
                impl->m_adapter->NotifyResetDocument();
            }

            AZ::SerializeContext::IDataContainer* m_container = nullptr;
            void* m_containerInstance = nullptr;
            void* m_elementInstance = nullptr;
        };

        struct ContainerEntry
        {
            AZStd::unique_ptr<BoundContainer> m_container;
            AZStd::unique_ptr<ContainerElement> m_element;
        };

        // Lookup table of containers and their elements for handling container operations
        AZ::Dom::DomPrefixTree<ContainerEntry> m_containers;

        ReflectionAdapterReflectionImpl(ReflectionAdapter* adapter)
            : m_adapter(adapter)
        {
            AZ::ComponentApplicationBus::BroadcastResult(m_serializeContext, &AZ::ComponentApplicationBus::Events::GetSerializeContext);
        }

        AZStd::string_view GetPropertyEditor(const Reflection::IAttributes& attributes)
        {
            auto handler = attributes.Find(Reflection::DescriptorAttributes::Handler);
            if (handler && handler->IsString())
            {
                return handler->GetString();
            }
            // Special case defaulting to ComboBox for enum types, as ComboBox isn't a default handler.
            if (auto enumTypeHandler = attributes.Find(Nodes::PropertyEditor::EnumType.GetName());
                enumTypeHandler && !enumTypeHandler->IsNull())
            {
                return Nodes::ComboBox::Name;
            }
            return {};
        }

        AZStd::string_view ExtractSerializedPath(const Reflection::IAttributes& attributes)
        {
            if (auto serializedPathAttribute = attributes.Find(Reflection::DescriptorAttributes::SerializedPath);
                serializedPathAttribute && serializedPathAttribute->IsString())
            {
                return serializedPathAttribute->GetString();
            }
            else
            {
                return {};
            }
        }

        void ExtractAndCreateLabel(const Reflection::IAttributes& attributes)
        {
            if (auto labelAttribute = attributes.Find(Reflection::DescriptorAttributes::Label);
                labelAttribute && labelAttribute->IsString())
            {
                AZStd::string_view serializedPath = ExtractSerializedPath(attributes);
                m_adapter->CreateLabel(&m_builder, labelAttribute->GetString(), serializedPath);
            }
        }

        void ForwardAttributes(const Reflection::IAttributes& attributes)
        {
            attributes.ListAttributes(
                [this](AZ::Name group, AZ::Name name, const Dom::Value& value)
                {
                    AZ_Warning("ReflectionAdapter", !name.IsEmpty(), "Received empty name in ListAttributes");
                    // Skip non-default groups, we don't presently source any attributes from outside of the default group.
                    if (!group.IsEmpty())
                    {
                        return;
                    }

                    const AZStd::array ignoredAttributes = { Reflection::DescriptorAttributes::Label,
                                                             Reflection::DescriptorAttributes::Handler,
                                                             Reflection::DescriptorAttributes::Container,
                                                             Nodes::PropertyEditor::Visibility.GetName() };
                    if (AZStd::ranges::find(ignoredAttributes, name) != ignoredAttributes.end())
                    {
                        return;
                    }

                    for (const auto& rowAttribute : Nodes::Row::RowAttributes)
                    {
                        if (name == rowAttribute->GetName())
                        {
                            return;
                        }
                    }

                    m_builder.Attribute(name, value);
                });
        }

        void VisitValue(
            Dom::Value value,
            void* instance,
            size_t valueSize,
            const Reflection::IAttributes& attributes,
            AZStd::function<Dom::Value(const Dom::Value&)> onChanged,
            bool createRow,
            bool hashValue)
        {
            if (createRow)
            {
                m_builder.BeginRow();
                ExtractAndCreateLabel(attributes);
            }

            m_builder.BeginPropertyEditor(GetPropertyEditor(attributes), AZStd::move(value));
            ForwardAttributes(attributes);
            m_onChangedCallbacks.SetValue(m_builder.GetCurrentPath(), AZStd::move(onChanged));
            m_builder.AddMessageHandler(m_adapter, Nodes::PropertyEditor::OnChanged);
            m_builder.AddMessageHandler(m_adapter, Nodes::PropertyEditor::RequestTreeUpdate);

            if (hashValue)
            {
                m_builder.Attribute(
                    Nodes::PropertyEditor::ValueHashed,
                    static_cast<AZ::u64>(AZStd::hash<AZ::Uuid>{}((AZ::Uuid::CreateData(static_cast<AZStd::byte*>(instance), valueSize)))));
            }
            m_builder.EndPropertyEditor();

            CheckContainerElement(instance, attributes);

            if (createRow)
            {
                m_builder.EndRow();
            }
        }

        void VisitValueWithSerializedPath(Reflection::IObjectAccess& access, const Reflection::IAttributes& attributes)
        {
            const AZ::TypeId valueType = access.GetType();
            void* valuePointer = access.Get();

            rapidjson::Document serializedValue;
            JsonSerialization::Store(serializedValue, serializedValue.GetAllocator(), valuePointer, nullptr, valueType);

            AZ::Dom::Value instancePointerValue;
            auto outputWriter = instancePointerValue.GetWriteHandler();
            auto convertToAzDomResult = AZ::Dom::Json::VisitRapidJsonValue(serializedValue, *outputWriter, AZ::Dom::Lifetime::Temporary);

            const AZ::Serialize::ClassData* classData = m_serializeContext->FindClassData(valueType);
            size_t typeSize = 0;
            if (classData)
            {
                if (AZ::IRttiHelper* rttiHelper = classData->m_azRtti; rttiHelper)
                {
                    typeSize = rttiHelper->GetTypeSize();
                }
            }

            VisitValue(
                instancePointerValue,
                valuePointer,
                typeSize,
                attributes,
                [valuePointer, valueType, this](const Dom::Value& newValue)
                {
                    AZ::JsonSerializationResult::ResultCode resultCode(AZ::JsonSerializationResult::Tasks::ReadField);
                    // marshal this new value into a pointer for use by the Json serializer if a pointer is being stored
                    if (auto marshalledPointer = AZ::Dom::Utils::TryMarshalValueToPointer(newValue, valueType);
                        marshalledPointer != nullptr)
                    {
                        rapidjson::Document buffer;
                        JsonSerializerSettings serializeSettings;
                        JsonDeserializerSettings deserializeSettings;
                        serializeSettings.m_serializeContext = m_serializeContext;
                        deserializeSettings.m_serializeContext = m_serializeContext;

                        // serialize the new value to Json, using the original valuePointer as a reference object to generate a minimal
                        // diff
                        resultCode = JsonSerialization::Store(
                            buffer, buffer.GetAllocator(), marshalledPointer, valuePointer, valueType, serializeSettings);

                        if (resultCode.GetProcessing() == AZ::JsonSerializationResult::Processing::Halted)
                        {
                            AZ_Error(
                                "ReflectionAdapter",
                                false,
                                "Storing new property editor value to JSON for copying to instance has failed with error %s",
                                resultCode.ToString("").c_str());
                            return newValue;
                        }

                        // now deserialize that value into the original location
                        resultCode = JsonSerialization::Load(valuePointer, valueType, buffer, deserializeSettings);
                        if (resultCode.GetProcessing() == AZ::JsonSerializationResult::Processing::Halted)
                        {
                            AZ_Error(
                                "ReflectionAdapter",
                                false,
                                "Loading JSON value containing new property editor into instance has failed with error %s",
                                resultCode.ToString("").c_str());
                            return newValue;
                        }
                    }
                    else
                    {
                        // Otherwise use Json Serialization to copy the Dom Value directly into the valuePointer address
                        resultCode = AZ::Dom::Utils::LoadViaJsonSerialization(valuePointer, valueType, newValue);
                        if (resultCode.GetProcessing() == AZ::JsonSerializationResult::Processing::Halted)
                        {
                            AZ_Error(
                                "ReflectionAdapter",
                                false,
                                "Loading new DOMValue directly into instance has failed with error %s",
                                resultCode.ToString("").c_str());
                            return newValue;
                        }
                    }

                    AZ::Dom::Value newInstancePointerValue;
                    AZ::JsonSerializerSettings storeSettings;
                    // Defaults must be kept to make sure a complete object is written to the Dom::Value
                    storeSettings.m_keepDefaults = true;
                    AZ::Dom::Utils::StoreViaJsonSerialization(valuePointer, nullptr, valueType, newInstancePointerValue, storeSettings);
                    return newInstancePointerValue;
                },
                false,
                false);
        }

        bool IsInspectorOverrideManagementEnabled()
        {
            bool isInspectorOverrideManagementEnabled = false;
            if (auto* console = AZ::Interface<AZ::IConsole>::Get(); console != nullptr)
            {
                console->GetCvarValue("ed_enableInspectorOverrideManagement", isInspectorOverrideManagementEnabled);
            }
            return isInspectorOverrideManagementEnabled;
        }

        template<class T>
        void VisitPrimitive(T& value, const Reflection::IAttributes& attributes)
        {
            Nodes::PropertyVisibility visibility = Nodes::PropertyVisibility::Show;

            if (auto visibilityAttribute = attributes.Find(Nodes::PropertyEditor::Visibility.GetName()); visibilityAttribute)
            {
                visibility = Nodes::PropertyEditor::Visibility.DomToValue(*visibilityAttribute).value_or(Nodes::PropertyVisibility::Show);
            }

            if (visibility == Nodes::PropertyVisibility::Hide || visibility == Nodes::PropertyVisibility::ShowChildrenOnly)
            {
                return;
            }
            VisitValue(
                Dom::Utils::ValueFromType(value),
                &value,
                sizeof(value),
                attributes,
                [&value](const Dom::Value& newValue)
                {
                    AZStd::optional<T> extractedValue = Dom::Utils::ValueToType<T>(newValue);
                    AZ_Warning("ReflectionAdapter", extractedValue.has_value(), "OnChanged failed, unable to extract value from DOM");
                    if (extractedValue.has_value())
                    {
                        value = AZStd::move(extractedValue.value());
                    }
                    return Dom::Utils::ValueFromType(value);
                },
                true,
                false);
        }

        void Visit(bool& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(char& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(AZ::s8& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(AZ::s16& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(AZ::s32& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(AZ::s64& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(AZ::u8& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(AZ::u16& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(AZ::u32& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(AZ::u64& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(float& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void Visit(double& value, const Reflection::IAttributes& attributes) override
        {
            VisitPrimitive(value, attributes);
        }

        void CreateContainerButton(
            Nodes::ContainerAction action,
            bool disabled = false,
            bool ancestorDisabled = false,
            AZ::s64 containerIndex = -1,
            AZ::DocumentPropertyEditor::Nodes::PropertyEditor::Align alignment = Nodes::PropertyEditor::Align::AlignRight)
        {
            m_builder.BeginPropertyEditor<Nodes::ContainerActionButton>();
            m_builder.Attribute(Nodes::PropertyEditor::SharePriorColumn, true);
            m_builder.Attribute(Nodes::PropertyEditor::UseMinimumWidth, true);
            m_builder.Attribute(Nodes::PropertyEditor::Alignment, alignment);
            m_builder.Attribute(Nodes::ContainerActionButton::Action, action);

            if (ancestorDisabled)
            {
                m_builder.Attribute(Nodes::PropertyEditor::AncestorDisabled, true);
            }
            if (disabled)
            {
                m_builder.Attribute(Nodes::PropertyEditor::Disabled, true);
            }
            if (containerIndex != -1)
            {
                m_builder.Attribute(Nodes::ContainerActionButton::ContainerIndex, containerIndex);
            }

            m_builder.AddMessageHandler(m_adapter, Nodes::ContainerActionButton::OnActivate.GetName());
            m_builder.EndPropertyEditor();
        }

        void CheckContainerElement(void* instance, const Reflection::IAttributes& attributes)
        {
            auto parentContainerAttribute = attributes.Find(AZ::Reflection::DescriptorAttributes::ParentContainer);
            auto parentContainerInstanceAttribute = attributes.Find(AZ::Reflection::DescriptorAttributes::ParentContainerInstance);

            AZ::Serialize::IDataContainer* parentContainer{};
            if (parentContainerAttribute && !parentContainerAttribute->IsNull())
            {
                if (auto parentContainerObject = AZ::Dom::Utils::ValueToType<AZ::PointerObject>(*parentContainerAttribute);
                    parentContainerObject && parentContainerObject->m_typeId == azrtti_typeid<AZ::Serialize::IDataContainer>())
                {
                    parentContainer = reinterpret_cast<AZ::Serialize::IDataContainer*>(parentContainerObject->m_address);
                }
            }

            void* parentContainerInstance{};
            if (parentContainer != nullptr && parentContainerInstanceAttribute && !parentContainerInstanceAttribute->IsNull())
            {
                if (auto parentContainerInstanceObject = AZ::Dom::Utils::ValueToType<AZ::PointerObject>(*parentContainerInstanceAttribute);
                    parentContainerInstanceObject && parentContainerInstanceObject->IsValid())
                {
                    parentContainerInstance = reinterpret_cast<AZ::Serialize::IDataContainer*>(parentContainerInstanceObject->m_address);
                }

                auto containerEntry = m_containers.ValueAtPath(m_builder.GetCurrentPath(), AZ::Dom::PrefixTreeMatch::ExactPath);
                if (containerEntry)
                {
                    containerEntry->m_element = ContainerElement::CreateContainerElement(instance, attributes);
                }
                else
                {
                    m_containers.SetValue(
                        m_builder.GetCurrentPath(),
                        ContainerEntry{ nullptr, ContainerElement::CreateContainerElement(instance, attributes) });
                }

                bool parentCanBeModified = true;
                if (auto parentCanBeModifiedValue = attributes.Find(AZ::Reflection::DescriptorAttributes::ParentContainerCanBeModified);
                    parentCanBeModifiedValue)
                {
                    parentCanBeModified = parentCanBeModifiedValue->IsBool() && parentCanBeModifiedValue->GetBool();
                }

                if (!parentContainer->IsFixedSize() && parentCanBeModified)
                {
                    bool isAncestorDisabledValue = false;
                    if (auto ancestorDisabledValue = attributes.Find(Nodes::NodeWithVisiblityControl::AncestorDisabled.GetName());
                        ancestorDisabledValue && ancestorDisabledValue->IsBool())
                    {
                        isAncestorDisabledValue = ancestorDisabledValue->GetBool();
                    }

                    if (parentContainerInstance)
                    {
                        auto containerSize = static_cast<AZ::s64>(parentContainer->Size(parentContainerInstance));
                        if (containerSize > 1 && parentContainer->IsSequenceContainer())
                        {
                            auto containerIndexAttribute = attributes.Find(AZ::Reflection::DescriptorAttributes::ContainerIndex);
                            AZ_Assert(
                                containerIndexAttribute && !containerIndexAttribute->IsNull(),
                                "children of a sequenced container should have a ContainerIndex!");
                            auto containerIndex = containerIndexAttribute->GetInt64();
                            CreateContainerButton(Nodes::ContainerAction::MoveUp, !containerIndex, isAncestorDisabledValue, containerIndex);
                            CreateContainerButton(
                                Nodes::ContainerAction::MoveDown,
                                containerIndex == containerSize - 1,
                                isAncestorDisabledValue,
                                containerIndex);
                        }
                    }
                    CreateContainerButton(Nodes::ContainerAction::RemoveElement, false, isAncestorDisabledValue);
                }
            }
        }

        // Check if the KeyValue attribute is set and if so create a property Editor for that key
        void CreatePropertyEditorForAssociativeContainerKey(
            const Reflection::IAttributes& attributes, ReflectionAdapter& adapter, AdapterBuilder& builder)
        {
            auto keyValueAttribute = attributes.Find(Nodes::PropertyEditor::KeyValue.GetName());
            // The element has no KeyValue attribute, so it is not part of an associative container therefore no work needs to be done
            if (keyValueAttribute == nullptr)
            {
                return;
            }

            if (auto keyValueEntry = AZ::Dom::Utils::ValueToType<AZ::Reflection::LegacyReflectionInternal::KeyEntry>(*keyValueAttribute);
                keyValueEntry&& keyValueEntry->IsValid())
            {
                AZ::PointerObject keyValuePointerObject = keyValueEntry->m_keyInstance;

                const AZStd::vector<AZ::Reflection::LegacyReflectionInternal::AttributeData>& keyAttributes = keyValueEntry->m_keyAttributes;

                // Create a lambda that can return a lambda that searches the keyAttributes vector for a specific attribute
                auto FindAttributeCreator = [](AZ::Name group, AZ::Name name)
                {
                    return [group, name](const AZ::Reflection::LegacyReflectionInternal::AttributeData& attributeData) -> bool
                    {
                        return group == attributeData.m_group&& name == attributeData.m_name;
                    };
                };

                AZStd::string_view keyPropertyHandlerName;
                // First try to search for the Handler attribute to see if a custom property handler has been specified
                if (auto handlerIt = AZStd::find_if(
                    keyAttributes.begin(),
                    keyAttributes.end(), FindAttributeCreator(AZ::Name{}, Reflection::DescriptorAttributes::Handler));
                    handlerIt != keyAttributes.end())
                {
                    const AZ::Dom::Value& handler = handlerIt->m_value;
                    if (handler.IsString())
                    {
                        keyPropertyHandlerName = handler.GetString();
                    }
                }

                if (keyPropertyHandlerName.empty())
                {
                    // If the Key doesn't have a custom property handler
                    // and it's type is an is represented by an enum use the combo box property handler
                    if (auto enumTypeHandlerIt = AZStd::find_if(
                        keyAttributes.begin(),
                        keyAttributes.end(),
                        FindAttributeCreator(AZ::Name{}, Nodes::PropertyEditor::EnumType.GetName()));
                        enumTypeHandlerIt != keyAttributes.end() && !enumTypeHandlerIt->m_value.IsNull())
                    {
                        keyPropertyHandlerName = Nodes::ComboBox::Name;
                    }
                }
                builder.BeginPropertyEditor(keyPropertyHandlerName, AZ::Dom::Utils::ValueFromType(keyValuePointerObject));
                builder.Attribute(Nodes::PropertyEditor::UseMinimumWidth, true);
                builder.Attribute(Nodes::PropertyEditor::Disabled, true);
                builder.AddMessageHandler(&adapter, Nodes::PropertyEditor::RequestTreeUpdate);
                builder.EndPropertyEditor();
            }
        }

        void VisitObjectBegin(Reflection::IObjectAccess& access, const Reflection::IAttributes& attributes) override
        {
            Nodes::PropertyVisibility visibility = Nodes::PropertyVisibility::Show;

            if (auto visibilityAttribute = attributes.Find(Nodes::PropertyEditor::Visibility.GetName()); visibilityAttribute)
            {
                visibility = Nodes::PropertyEditor::Visibility.DomToValue(*visibilityAttribute).value_or(Nodes::PropertyVisibility::Show);
            }

            if (visibility == Nodes::PropertyVisibility::Hide || visibility == Nodes::PropertyVisibility::ShowChildrenOnly)
            {
                return;
            }

            m_builder.BeginRow();

            for (const auto& attribute : Nodes::Row::RowAttributes)
            {
                if (auto attributeValue = attributes.Find(attribute->GetName()); attributeValue && !attributeValue->IsNull())
                {
                    m_builder.Attribute(attribute->GetName(), *attributeValue);
                }
            }

            if (access.GetType() == azrtti_typeid<AZStd::string>())
            {
                ExtractAndCreateLabel(attributes);

                AZStd::string& value = *reinterpret_cast<AZStd::string*>(access.Get());
                VisitValue(
                    Dom::Utils::ValueFromType(value),
                    &value,
                    sizeof(value),
                    attributes,
                    [&value](const Dom::Value& newValue)
                    {
                        value = newValue.GetString();
                        return newValue;
                    },
                    false,
                    false);
                return;
            }
            else if (access.GetType() == azrtti_typeid<bool>())
            {
                // handle bool pointers directly for elements like group toggles
                ExtractAndCreateLabel(attributes);

                bool& value = *reinterpret_cast<bool*>(access.Get());
                VisitValue(
                    Dom::Utils::ValueFromType(value),
                    &value,
                    sizeof(value),
                    attributes,
                    [&value](const Dom::Value& newValue)
                    {
                        value = newValue.GetBool();
                        return newValue;
                    },
                    false,
                    false);
                return;
            }
            else
            {
                auto containerAttribute = attributes.Find(Reflection::DescriptorAttributes::Container);

                AZ::Serialize::IDataContainer* container{};
                if (containerAttribute && !containerAttribute->IsNull())
                {
                    if (auto containerObject = AZ::Dom::Utils::ValueToType<AZ::PointerObject>(*containerAttribute);
                        containerObject && containerObject->m_typeId == azrtti_typeid<AZ::Serialize::IDataContainer>())
                    {
                        container = reinterpret_cast<AZ::Serialize::IDataContainer*>(containerObject->m_address);
                    }
                }
                if (container != nullptr)
                {
                    m_containers.SetValue(
                        m_builder.GetCurrentPath(), ContainerEntry{ BoundContainer::CreateBoundContainer(access.Get(), attributes) });

                    auto labelAttribute = attributes.Find(Reflection::DescriptorAttributes::Label);
                    if (labelAttribute && !labelAttribute->IsNull() && labelAttribute->IsString())
                    {
                        AZStd::string_view serializedPath = ExtractSerializedPath(attributes);

                        m_adapter->CreateLabel(&m_builder, labelAttribute->GetString(), serializedPath);

                        auto valueTextAttribute = attributes.Find(Nodes::Label::ValueText.GetName());
                        if (valueTextAttribute && !valueTextAttribute->IsNull() && valueTextAttribute->IsString())
                        {
                            m_adapter->CreateLabel(&m_builder, valueTextAttribute->GetString(), serializedPath);
                        }
                        else
                        {
                            size_t containerSize = container->Size(access.Get());
                            if (containerSize == 1)
                            {
                                m_adapter->CreateLabel(&m_builder, AZStd::string::format("1 element"), serializedPath);
                            }
                            else
                            {
                                m_adapter->CreateLabel(&m_builder, AZStd::string::format("%zu elements", containerSize), serializedPath);
                            }
                        }
                    }

                    bool canBeModified = true;
                    if (auto canBeModifiedValue = attributes.Find(Nodes::Container::ContainerCanBeModified.GetName()); canBeModifiedValue)
                    {
                        canBeModified = canBeModifiedValue->IsBool() && canBeModifiedValue->GetBool();
                    }

                    if (canBeModified && !container->IsFixedSize())
                    {
                        bool isDisabled = false;
                        if (auto disabledValue = attributes.Find(Nodes::NodeWithVisiblityControl::Disabled.GetName()); disabledValue)
                        {
                            isDisabled = disabledValue->IsBool() && disabledValue->GetBool();
                        }
                        CreateContainerButton(Nodes::ContainerAction::AddElement, isDisabled);

                        if (!isDisabled)
                        {
                            // disable the clear button if the container is already empty
                            isDisabled = (container->Size(access.Get()) == 0);
                        }
                        CreateContainerButton(Nodes::ContainerAction::Clear, isDisabled);
                    }
                }
                else
                {
                    ExtractAndCreateLabel(attributes);
                }

                AZ::Dom::Value instancePointerValue = AZ::Dom::Utils::MarshalTypedPointerToValue(access.Get(), access.GetType());
                // Only hash an opaque value
                // A value that is not opaque, but is a pointer will have it's members visited in a recursive call to this method
                const bool hashValue = instancePointerValue.IsOpaqueValue();

                // The IsInspectorOverrideManagementEnabled() check is only temporary until the inspector override management feature set
                // is fully developed. Since the original utils funtion is in AzToolsFramework and we can't access it from here, we are
                // duplicating it in this class temporarily till we can do more testing and gain confidence about this new way of storing
                // serialized values of opaque types directly in the DPE DOM.
                AZStd::string_view serializedPath = ExtractSerializedPath(attributes);
                if (IsInspectorOverrideManagementEnabled() && !serializedPath.empty())
                {
                    VisitValueWithSerializedPath(access, attributes);
                }
                else
                {
                    const AZ::Serialize::ClassData* classData = m_serializeContext->FindClassData(access.GetType());
                    size_t typeSize = 0;
                    if (classData)
                    {
                        if (AZ::IRttiHelper* rttiHelper = classData->m_azRtti; rttiHelper)
                        {
                            typeSize = rttiHelper->GetTypeSize();
                        }
                    }

                    // this needs to write the value back into the reflected object via Json serialization
                    auto StoreValueIntoPointer =
                        [valuePointer = access.Get(), valueType = access.GetType(), this](const Dom::Value& newValue)
                    {
                        AZ::JsonSerializationResult::ResultCode resultCode(AZ::JsonSerializationResult::Tasks::ReadField);
                        // marshal this new value into a pointer for use by the Json serializer if a pointer is being stored
                        if (auto marshalledPointer = AZ::Dom::Utils::TryMarshalValueToPointer(newValue, valueType);
                            marshalledPointer != nullptr)
                        {
                            rapidjson::Document buffer;
                            JsonSerializerSettings serializeSettings;
                            JsonDeserializerSettings deserializeSettings;
                            serializeSettings.m_serializeContext = m_serializeContext;
                            deserializeSettings.m_serializeContext = m_serializeContext;

                            // serialize the new value to Json, using the original valuePointer as a reference object to generate a minimal
                            // diff
                            resultCode = JsonSerialization::Store(
                                buffer, buffer.GetAllocator(), marshalledPointer, valuePointer, valueType, serializeSettings);

                            if (resultCode.GetProcessing() == AZ::JsonSerializationResult::Processing::Halted)
                            {
                                AZ_Error(
                                    "ReflectionAdapter",
                                    false,
                                    "Storing new property editor value to JSON for copying to instance has failed with error %s",
                                    resultCode.ToString("").c_str());
                                return newValue;
                            }

                            // now deserialize that value into the original location
                            resultCode = JsonSerialization::Load(valuePointer, valueType, buffer, deserializeSettings);
                            if (resultCode.GetProcessing() == AZ::JsonSerializationResult::Processing::Halted)
                            {
                                AZ_Error(
                                    "ReflectionAdapter",
                                    false,
                                    "Loading JSON value containing new property editor into instance has failed with error %s",
                                    resultCode.ToString("").c_str());
                                return newValue;
                            }
                        }
                        else
                        {
                            // Otherwise use Json Serialization to copy the Dom Value directly into the valuePointer address
                            resultCode = AZ::Dom::Utils::LoadViaJsonSerialization(valuePointer, valueType, newValue);
                            if (resultCode.GetProcessing() == AZ::JsonSerializationResult::Processing::Halted)
                            {
                                AZ_Error(
                                    "ReflectionAdapter",
                                    false,
                                    "Loading new DOMValue directly into instance has failed with error %s",
                                    resultCode.ToString("").c_str());
                                return newValue;
                            }
                        }

                        // NB: the returned value for serialized pointer values is instancePointerValue, but since this is passed by
                        // pointer, it will not actually detect a changed dom value. Since we are already writing directly to the DOM
                        // before this step, it won't affect the calling DPE, however, other DPEs pointed at the same adapter would be
                        // unaware of the change, and wouldn't update their UI. In future, to properly support multiple DPEs on one
                        // adapter, we will need to solve this. One way would be to store the json serialized value (which is mostly
                        // human-readable text) as an attribute, so any change to the Json would trigger an update. This would have the
                        // advantage of allowing opaque and pointer types to be searchable by the string-based Filter adapter. Without
                        // this, things like Vector3 will not have searchable values by text. These advantages would have to be measured
                        // against the size changes in the DOM and the time taken to populate and parse them.
                        return newValue;
                    };
                    void* instance = access.Get();
                    VisitValue(instancePointerValue, instance, typeSize, attributes, AZStd::move(StoreValueIntoPointer), false, hashValue);
                }
            }
        }

        void VisitObjectEnd([[maybe_unused]] Reflection::IObjectAccess& access, const Reflection::IAttributes& attributes) override
        {
            Nodes::PropertyVisibility visibility = Nodes::PropertyVisibility::Show;

            if (auto visibilityAttribute = attributes.Find(Nodes::PropertyEditor::Visibility.GetName()); visibilityAttribute)
            {
                visibility = Nodes::PropertyEditor::Visibility.DomToValue(*visibilityAttribute).value_or(Nodes::PropertyVisibility::Show);
            }

            if (visibility == Nodes::PropertyVisibility::Hide || visibility == Nodes::PropertyVisibility::ShowChildrenOnly)
            {
                return;
            }
            m_builder.EndRow();
        }

        void Visit(
            [[maybe_unused]] const AZStd::string_view value,
            [[maybe_unused]] Reflection::IStringAccess& access,
            [[maybe_unused]] const Reflection::IAttributes& attributes) override
        {
        }

        void Visit([[maybe_unused]] Reflection::IArrayAccess& access, [[maybe_unused]] const Reflection::IAttributes& attributes) override
        {
        }

        void Visit([[maybe_unused]] Reflection::IMapAccess& access, [[maybe_unused]] const Reflection::IAttributes& attributes) override
        {
        }

        void Visit(
            [[maybe_unused]] Reflection::IDictionaryAccess& access, [[maybe_unused]] const Reflection::IAttributes& attributes) override
        {
        }

        void Visit(
            [[maybe_unused]] AZ::s64 value,
            [[maybe_unused]] const Reflection::IEnumAccess& access,
            [[maybe_unused]] const Reflection::IAttributes& attributes) override
        {
        }

        void Visit([[maybe_unused]] Reflection::IPointerAccess& access, [[maybe_unused]] const Reflection::IAttributes& attributes) override
        {
        }

        void Visit([[maybe_unused]] Reflection::IBufferAccess& access, [[maybe_unused]] const Reflection::IAttributes& attributes) override
        {
        }

        void Visit(
            [[maybe_unused]] const AZ::Data::Asset<AZ::Data::AssetData>& asset,
            [[maybe_unused]] Reflection::IAssetAccess& access,
            [[maybe_unused]] const Reflection::IAttributes& attributes) override
        {
        }
    };

    ReflectionAdapter::ReflectionAdapter()
        : RoutingAdapter()
        , m_impl(AZStd::make_unique<ReflectionAdapterReflectionImpl>(this))
    {
    }

    ReflectionAdapter::ReflectionAdapter(void* instance, AZ::TypeId typeId)
        : RoutingAdapter()
        , m_impl(AZStd::make_unique<ReflectionAdapterReflectionImpl>(this))
    {
        SetValue(instance, AZStd::move(typeId));
    }

    // Declare dtor in implementation to make the unique_ptr deleter for m_impl accessible
    ReflectionAdapter::~ReflectionAdapter() = default;

    void ReflectionAdapter::SetValue(void* instance, AZ::TypeId typeId)
    {
        m_instance = instance;
        m_typeId = AZStd::move(typeId);

        // new top-value, do a full reset
        NotifyResetDocument(DocumentResetType::HardReset);
    }

    void ReflectionAdapter::InvokeChangeNotify(const AZ::Dom::Value& domNode)
    {
        using Nodes::PropertyEditor;
        using Nodes::PropertyRefreshLevel;

        // Trigger ChangeNotify
        auto changeNotify = PropertyEditor::ChangeNotify.InvokeOnDomNode(domNode);
        if (changeNotify.IsSuccess())
        {
            // If we were told to issue a property refresh, notify our adapter via RequestTreeUpdate
            PropertyRefreshLevel level = changeNotify.GetValue();
            if (level != PropertyRefreshLevel::Undefined && level != PropertyRefreshLevel::None)
            {
                PropertyEditor::RequestTreeUpdate.InvokeOnDomNode(domNode, level);
            }
        }
    }

    void ReflectionAdapter::ConnectPropertyChangeHandler(PropertyChangeEvent::Handler& handler)
    {
        handler.Connect(m_propertyChangeEvent);
    }

    void ReflectionAdapter::NotifyPropertyChanged(const PropertyChangeInfo& changeInfo)
    {
        m_propertyChangeEvent.Signal(changeInfo);
    }

    void ReflectionAdapter::CreateLabel(
        AdapterBuilder* adapterBuilder, AZStd::string_view labelText, [[maybe_unused]] AZStd::string_view serializedPath)
    {
        adapterBuilder->Label(labelText);
    }

    void ReflectionAdapter::UpdateDomContents(const PropertyChangeInfo& propertyChangeInfo)
    {
        NotifyContentsChanged({ Dom::PatchOperation::ReplaceOperation(propertyChangeInfo.path / "Value", propertyChangeInfo.newValue) });
    }

    ExpanderSettings* ReflectionAdapter::CreateExpanderSettings(
        DocumentAdapter* referenceAdapter, const AZStd::string& settingsRegistryKey, const AZStd::string& propertyEditorName)
    {
        return new LabeledRowDPEExpanderSettings(referenceAdapter, settingsRegistryKey, propertyEditorName);
    }

    Dom::Value ReflectionAdapter::GenerateContents()
    {
        m_impl->m_builder.BeginAdapter();
        m_impl->m_builder.AddMessageHandler(this, Nodes::Adapter::QueryKey);
        m_impl->m_builder.AddMessageHandler(this, Nodes::Adapter::AddContainerKey);
        m_impl->m_builder.AddMessageHandler(this, Nodes::Adapter::RejectContainerKey);
        m_impl->m_builder.AddMessageHandler(this, Nodes::Adapter::SetNodeDisabled);
        m_impl->m_onChangedCallbacks.Clear();
        m_impl->m_containers.Clear();
        if (m_instance != nullptr)
        {
            Reflection::VisitLegacyInMemoryInstance(m_impl.get(), m_instance, m_typeId);
        }
        m_impl->m_builder.EndAdapter();
        return m_impl->m_builder.FinishAndTakeResult();
    }

    Dom::Value ReflectionAdapter::HandleMessage(const AdapterMessage& message)
    {
        auto handlePropertyEditorChanged = [&](const Dom::Value& valueFromEditor, Nodes::ValueChangeType changeType)
        {
            auto changeHandler = m_impl->m_onChangedCallbacks.ValueAtPath(message.m_messageOrigin, AZ::Dom::PrefixTreeMatch::ExactPath);
            if (changeHandler != nullptr)
            {
                Dom::Value newValue = (*changeHandler)(valueFromEditor);
                UpdateDomContents({ message.m_messageOrigin, newValue, changeType });
                NotifyPropertyChanged({ message.m_messageOrigin, newValue, changeType });
            }
        };

        auto handleSetNodeDisabled = [&](bool shouldDisable, Dom::Path targetNodePath)
        {
            Dom::Value targetNode = GetContents()[targetNodePath];

            if (!targetNode.IsNode() || targetNode.IsNull())
            {
                AZ_Warning(
                    "ReflectionAdapter",
                    false,
                    "Failed to update disabled state for Value at path `%s`; this is not a valid node",
                    targetNodePath.ToString().c_str());
                return;
            }

            const Name& disabledAttributeName = Nodes::NodeWithVisiblityControl::Disabled.GetName();
            const Name& ancestorDisabledAttrName = Nodes::NodeWithVisiblityControl::AncestorDisabled.GetName();

            Dom::Patch patch;
            AZStd::stack<AZStd::pair<Dom::Path, const Dom::Value*>> unvisitedDescendants;

            const auto queueDescendantsForSearch = [&unvisitedDescendants](const Dom::Value& parentNode, const Dom::Path& parentPath)
            {
                int index = 0;
                for (auto child = parentNode.ArrayBegin(); child != parentNode.ArrayEnd(); ++child)
                {
                    if (child->IsNode())
                    {
                        unvisitedDescendants.push({ parentPath / index, child });
                    }
                    ++index;
                }
            };

            const auto propagateAttributeChangeToRow = [&](const Dom::Value& parentNode,
                                                           const Dom::Path& parentPath,
                                                           AZStd::function<void(const Dom::Value&, const Dom::Path&)> procedure)
            {
                int index = 0;
                for (auto child = parentNode.ArrayBegin(); child != parentNode.ArrayEnd(); ++child)
                {
                    if (child->IsNode())
                    {
                        auto childPath = parentPath / index;
                        if (child->GetNodeName() != GetNodeName<Nodes::Row>())
                        {
                            procedure(*child, childPath);
                        }
                        queueDescendantsForSearch(*child, childPath);
                    }
                    ++index;
                }
            };

            // This lambda applies the attribute change to any descendants in unvisitedChildren until its done
            const auto propagateAttributeChangeToDescendants = [&](AZStd::function<void(const Dom::Value&, Dom::Path&)> procedure)
            {
                while (!unvisitedDescendants.empty())
                {
                    Dom::Path nodePath = unvisitedDescendants.top().first;
                    auto node = unvisitedDescendants.top().second;
                    unvisitedDescendants.pop();

                    if (node->GetNodeName() != GetNodeName<Nodes::Row>())
                    {
                        procedure(*node, nodePath);
                    }

                    // We can stop traversing this path if the node has a truthy disabled attribute since its descendants
                    // should retain their inherited disabled state
                    if (auto iter = node->FindMember(disabledAttributeName); iter == node->MemberEnd() || !iter->second.GetBool())
                    {
                        queueDescendantsForSearch(*node, nodePath);
                    }
                }
            };

            if (shouldDisable)
            {
                if (targetNode.GetNodeName() == GetNodeName<Nodes::Row>())
                {
                    propagateAttributeChangeToRow(
                        targetNode,
                        targetNodePath,
                        [&patch, &disabledAttributeName](const Dom::Value& node, const Dom::Path& nodePath)
                        {
                            if (auto iter = node.FindMember(disabledAttributeName); iter == node.MemberEnd() || !iter->second.GetBool())
                            {
                                patch.PushBack({ Dom::PatchOperation::AddOperation(nodePath / disabledAttributeName, Dom::Value(true)) });
                            }
                        });
                }
                else
                {
                    patch.PushBack({ Dom::PatchOperation::AddOperation(targetNodePath / disabledAttributeName, Dom::Value(true)) });
                    queueDescendantsForSearch(targetNode, targetNodePath);
                }

                propagateAttributeChangeToDescendants(
                    [&patch, &ancestorDisabledAttrName](const Dom::Value& node, const Dom::Path& nodePath)
                    {
                        if (auto iter = node.FindMember(ancestorDisabledAttrName); iter == node.MemberEnd() || !iter->second.GetBool())
                        {
                            patch.PushBack({ Dom::PatchOperation::AddOperation(nodePath / ancestorDisabledAttrName, Dom::Value(true)) });
                        }
                    });
            }
            else
            {
                if (targetNode.GetNodeName() == GetNodeName<Nodes::Row>())
                {
                    propagateAttributeChangeToRow(
                        targetNode,
                        targetNodePath,
                        [&patch, &disabledAttributeName](const Dom::Value& node, const Dom::Path& nodePath)
                        {
                            if (auto iter = node.FindMember(disabledAttributeName); iter != node.MemberEnd() && iter->second.GetBool())
                            {
                                patch.PushBack({ Dom::PatchOperation::RemoveOperation(nodePath / disabledAttributeName) });
                            }
                        });
                }
                else
                {
                    patch.PushBack({ Dom::PatchOperation::RemoveOperation(targetNodePath / disabledAttributeName) });
                    queueDescendantsForSearch(targetNode, targetNodePath);
                }

                propagateAttributeChangeToDescendants(
                    [&patch, &ancestorDisabledAttrName](const Dom::Value& node, const Dom::Path& nodePath)
                    {
                        if (auto iter = node.FindMember(ancestorDisabledAttrName); iter != node.MemberEnd() && iter->second.GetBool())
                        {
                            patch.PushBack({ Dom::PatchOperation::RemoveOperation(nodePath / ancestorDisabledAttrName) });
                        }
                    });
            }

            if (patch.Size() > 0)
            {
                NotifyContentsChanged(patch);
            }
        };

        auto handleContainerOperation = [&]()
        {
            if (message.m_messageOrigin.Size() == 0)
            {
                return;
            }
            auto containerEntry = m_impl->m_containers.ValueAtPath(message.m_messageOrigin, AZ::Dom::PrefixTreeMatch::ParentsOnly);
            if (containerEntry != nullptr)
            {
                using Nodes::ContainerAction;
                AZ::Dom::Value node = GetContents()[message.m_messageOrigin];
                auto action = Nodes::ContainerActionButton::Action.ExtractFromDomNode(node);
                if (!action.has_value())
                {
                    return;
                }
                switch (action.value())
                {
                case ContainerAction::AddElement:
                    if (containerEntry->m_container)
                    {
                        containerEntry->m_container->OnAddElement(m_impl.get(), message.m_messageOrigin);
                    }
                    break;
                case ContainerAction::RemoveElement:
                    if (containerEntry->m_element)
                    {
                        containerEntry->m_element->OnRemoveElement(m_impl.get(), message.m_messageOrigin);
                    }
                    break;
                case ContainerAction::Clear:
                    if (containerEntry->m_container)
                    {
                        containerEntry->m_container->OnClear(m_impl.get(), message.m_messageOrigin);
                    }
                    break;
                case ContainerAction::MoveUp:
                case ContainerAction::MoveDown:
                    if (containerEntry->m_element)
                    {
                        auto containerIndex = Nodes::ContainerActionButton::ContainerIndex.ExtractFromDomNode(node);
                        AZ_Assert(containerIndex.has_value(), "MoveUp and MoveDown actions must have a ContainerIndex!");
                        containerEntry->m_element->OnMoveElement(
                            m_impl.get(), message.m_messageOrigin, containerIndex.value(), action.value() == ContainerAction::MoveDown);
                    }
                    break;
                }
            }
        };

        auto addKeyToContainer = [&](AZ::DocumentPropertyEditor::DocumentAdapterPtr* adapter, AZ::Dom::Path containerPath)
        {
            auto containerEntry = m_impl->m_containers.ValueAtPath(containerPath, AZ::Dom::PrefixTreeMatch::ParentsOnly);
            if (containerEntry->m_container)
            {
                containerEntry->m_container->OnAddElementToAssociativeContainer(m_impl.get(), adapter, containerPath);
            }
        };

        auto rejectKeyToContainer = [&](AZ::Dom::Path containerPath)
        {
            auto containerEntry = m_impl->m_containers.ValueAtPath(containerPath, AZ::Dom::PrefixTreeMatch::ParentsOnly);
            if (containerEntry->m_container)
            {
                containerEntry->m_container->RejectAssociativeContainerKey(m_impl.get());
            }
        };

        auto handleTreeUpdate = [&](Nodes::PropertyRefreshLevel)
        {
            // For now just trigger a soft reset but the end goal is to handle granular updates.
            // This will still only send the view patches for what's actually changed.
            NotifyResetDocument();
        };

        return message.Match(
            Nodes::PropertyEditor::OnChanged,
            handlePropertyEditorChanged,
            Nodes::ContainerActionButton::OnActivate,
            handleContainerOperation,
            Nodes::PropertyEditor::RequestTreeUpdate,
            handleTreeUpdate,
            Nodes::Adapter::SetNodeDisabled,
            handleSetNodeDisabled,
            Nodes::Adapter::AddContainerKey,
            addKeyToContainer,
            Nodes::Adapter::RejectContainerKey,
            rejectKeyToContainer);
    }
} // namespace AZ::DocumentPropertyEditor
