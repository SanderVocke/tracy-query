use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, ItemFn, ReturnType, Type};

#[proc_macro_attribute]
pub fn tracy_capture_test(arguments: TokenStream, input: TokenStream) -> TokenStream {
    if !arguments.is_empty() {
        return syn::Error::new(proc_macro2::Span::call_site(), "tracy_capture_test takes no arguments")
            .into_compile_error()
            .into();
    }
    let mut function = parse_macro_input!(input as ItemFn);
    if function.sig.asyncness.is_some() {
        return syn::Error::new_spanned(&function.sig, "async tests are not supported")
            .into_compile_error()
            .into();
    }
    if function.attrs.iter().any(|attribute| attribute.path().is_ident("should_panic")) {
        return syn::Error::new_spanned(&function.sig, "#[should_panic] is not supported")
            .into_compile_error()
            .into();
    }
    if !function.sig.inputs.is_empty() || function.sig.generics.params.len() != 0 {
        return syn::Error::new_spanned(&function.sig, "captured tests must have no parameters or generics")
            .into_compile_error()
            .into();
    }

    let returns_result = match &function.sig.output {
        ReturnType::Default => false,
        ReturnType::Type(_, output) => match output.as_ref() {
            Type::Tuple(tuple) if tuple.elems.is_empty() => false,
            Type::Path(path) if path.path.segments.last().is_some_and(|segment| segment.ident == "Result") => true,
            _ => {
                return syn::Error::new_spanned(
                    output,
                    "captured tests must return () or Result<(), E>",
                )
                .into_compile_error()
                .into()
            }
        },
    };

    let body = function.block;
    function.attrs.retain(|attribute| !attribute.path().is_ident("test"));
    function.attrs.push(syn::parse_quote!(#[test]));
    function.block = if returns_result {
        Box::new(syn::parse_quote!({
            ::tracy_nextest_capture::run_result(|| #body)
        }))
    } else {
        Box::new(syn::parse_quote!({
            ::tracy_nextest_capture::run(|| #body)
        }))
    };
    quote!(#function).into()
}
