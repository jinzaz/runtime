// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Dynamic;
using Microsoft.CSharp.RuntimeBinder.Errors;
using Microsoft.CSharp.RuntimeBinder.Semantics;

namespace Microsoft.CSharp.RuntimeBinder
{
    /// <summary>
    /// Represents a dynamic indexer access in C#, providing the binding semantics and the details about the operation.
    /// Instances of this class are generated by the C# compiler.
    /// </summary>
    [RequiresUnreferencedCode(Binder.TrimmerWarning)]
    [RequiresDynamicCode(Binder.DynamicCodeWarning)]
    internal sealed class CSharpGetIndexBinder : GetIndexBinder, ICSharpBinder
    {
        public string Name => SpecialNames.Indexer;

        public BindingFlag BindingFlags => BindingFlag.BIND_RVALUEREQUIRED;

        public Expr DispatchPayload(RuntimeBinder runtimeBinder, ArgumentObject[] arguments, LocalVariableSymbol[] locals)
        {
            Expr indexerArguments = runtimeBinder.CreateArgumentListEXPR(arguments, locals, 1, arguments.Length);
            return runtimeBinder.BindProperty(this, arguments[0], locals[0], indexerArguments);
        }

        public void PopulateSymbolTableWithName(Type callingType, ArgumentObject[] arguments)
            => SymbolTable.PopulateSymbolTableWithName(SpecialNames.Indexer, null, arguments[0].Type);

        public bool IsBinderThatCanHaveRefReceiver => true;

        private readonly CSharpArgumentInfo[] _argumentInfo;

        CSharpArgumentInfo ICSharpBinder.GetArgumentInfo(int index) => _argumentInfo[index];

        private readonly RuntimeBinder _binder;

        private readonly Type _callingContext;

        /// <summary>
        /// Initializes a new instance of the <see cref="CSharpGetIndexBinder" />.
        /// </summary>
        /// <param name="callingContext">The <see cref="System.Type"/> that indicates where this operation is defined.</param>
        /// <param name="argumentInfo">The sequence of <see cref="CSharpArgumentInfo"/> instances for the arguments to this operation.</param>
        public CSharpGetIndexBinder(
            Type callingContext,
            IEnumerable<CSharpArgumentInfo> argumentInfo) :
            base(BinderHelper.CreateCallInfo(ref argumentInfo, 1)) // discard 1 argument: the target object
        {
            _argumentInfo = argumentInfo as CSharpArgumentInfo[];
            _callingContext = callingContext;
            _binder = new RuntimeBinder(callingContext);
        }

        public int GetGetBinderEquivalenceHash()
        {
            int hash = _callingContext?.GetHashCode() ?? 0;
            hash = BinderHelper.AddArgHashes(hash, _argumentInfo);

            return hash;
        }

        public bool IsEquivalentTo(ICSharpBinder other)
        {
            var otherBinder = other as CSharpGetIndexBinder;
            if (otherBinder == null)
            {
                return false;
            }

            if (_callingContext != otherBinder._callingContext ||
                _argumentInfo.Length != otherBinder._argumentInfo.Length)
            {
                return false;
            }

            return BinderHelper.CompareArgInfos(_argumentInfo, otherBinder._argumentInfo);
        }

        /// <summary>
        /// Performs the binding of the dynamic get index operation if the target dynamic object cannot bind.
        /// </summary>
        /// <param name="target">The target of the dynamic get index operation.</param>
        /// <param name="indexes">The arguments of the dynamic get index operation.</param>
        /// <param name="errorSuggestion">The binding result to use if binding fails, or null.</param>
        /// <returns>The <see cref="DynamicMetaObject"/> representing the result of the binding.</returns>
        public override DynamicMetaObject FallbackGetIndex(DynamicMetaObject target, DynamicMetaObject[] indexes, DynamicMetaObject errorSuggestion)
        {
#if ENABLECOMBINDER
            DynamicMetaObject com;
            if (ComInterop.ComBinder.TryBindGetIndex(this, target, indexes, out com))
            {
                return com;
            }
#else
            BinderHelper.ThrowIfUsingDynamicCom(target);
#endif

            BinderHelper.ValidateBindArgument(target, nameof(target));
            BinderHelper.ValidateBindArgument(indexes, nameof(indexes));
            return BinderHelper.Bind(this, _binder, BinderHelper.Cons(target, indexes), _argumentInfo, errorSuggestion);
        }
    }
}
