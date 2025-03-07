#include "marian.h"

#include "common/fastopt.h"

#include "models/model_factory.h"
#include "models/encoder_decoder.h"
#include "models/encoder_classifier.h"
#include "models/bert.h"

#include "models/costs.h"

#include "models/amun.h"
#include "models/nematus.h"
#include "models/s2s.h"
#include "models/laser.h"
#include "models/transformer_factory.h"
#include "models/transformer_new.h"

#include "models/comet_qe.h"
#include "models/bleurt.h"

#ifdef CUDNN
#include "models/char_s2s.h"
#endif

#ifdef COMPILE_EXAMPLES
#include "examples/mnist/model.h"
#ifdef CUDNN
#include "examples/mnist/model_lenet.h"
#endif
#endif

namespace marian {
namespace models {

Ptr<EncoderBase> EncoderFactory::construct(Ptr<ExpressionGraph> graph) {
  if(options_->get<std::string>("type") == "s2s")
    return New<EncoderS2S>(graph, options_);
  
  if(options_->get<std::string>("type") == "laser" || options_->get<std::string>("type") == "laser-sim")
    return New<EncoderLaser>(graph, options_);

#ifdef CUDNN
  if(options_->get<std::string>("type") == "char-s2s")
    return New<CharS2SEncoder>(graph, options_);
#endif

  if(options_->get<std::string>("type") == "transformer")
    return NewEncoderTransformer(graph, options_);

  if(options_->get<std::string>("type") == "bert-encoder")
    return New<BertEncoder>(graph, options_);

  ABORT("Unknown encoder type {}", options_->get<std::string>("type"));
}

Ptr<DecoderBase> DecoderFactory::construct(Ptr<ExpressionGraph> graph) {
  if(options_->get<std::string>("type") == "s2s")
    return New<DecoderS2S>(graph, options_);
  if(options_->get<std::string>("type") == "transformer")
    return NewDecoderTransformer(graph, options_);
  ABORT("Unknown decoder type");
}

Ptr<ClassifierBase> ClassifierFactory::construct(Ptr<ExpressionGraph> graph) {
  if(options_->get<std::string>("type") == "bert-masked-lm")
    return New<BertMaskedLM>(graph, options_);
  else if(options_->get<std::string>("type") == "bert-classifier")
    return New<BertClassifier>(graph, options_);
  else
    ABORT("Unknown classifier type");
}

Ptr<PoolerBase> PoolerFactory::construct(Ptr<ExpressionGraph> graph) {
  if(options_->get<std::string>("type") == "max-pooler")
    return New<MaxPooler>(graph, options_);
  else if(options_->get<std::string>("type") == "slice-pooler")
    return New<SlicePooler>(graph, options_);
  else if(options_->get<std::string>("type") == "sim-pooler")
    return New<SimPooler>(graph, options_);
  else
    ABORT("Unknown pooler type");
}

Ptr<IModel> EncoderDecoderFactory::construct(Ptr<ExpressionGraph> graph) {
  Ptr<EncoderDecoder> encdec;
  if(options_->get<std::string>("type") == "amun")
    encdec = New<Amun>(graph, options_);
  else if(options_->get<std::string>("type") == "nematus")
    encdec = New<Nematus>(graph, options_);
  else
    encdec = New<EncoderDecoder>(graph, options_);

  for(auto& ef : encoders_)
    encdec->push_back(ef(options_).construct(graph));

  for(auto& df : decoders_)
    encdec->push_back(df(options_).construct(graph));

  return encdec;
}

Ptr<IModel> EncoderClassifierFactory::construct(Ptr<ExpressionGraph> graph) {
  Ptr<EncoderClassifier> enccls;
  if(options_->get<std::string>("type") == "bert")
    enccls = New<BertEncoderClassifier>(options_);
  else if(options_->get<std::string>("type") == "bert-classifier")
    enccls = New<BertEncoderClassifier>(options_);
  else
    enccls = New<EncoderClassifier>(options_);

  for(auto& ef : encoders_)
    enccls->push_back(ef(options_).construct(graph));

  for(auto& cf : classifiers_)
    enccls->push_back(cf(options_).construct(graph));

  return enccls;
}

Ptr<IModel> EncoderPoolerFactory::construct(Ptr<ExpressionGraph> graph) {
  Ptr<EncoderPooler> encpool = New<EncoderPooler>(options_);

  for(auto& ef : encoders_)
    encpool->push_back(ef(options_).construct(graph));

  for(auto& pl : poolers_)
    encpool->push_back(pl(options_).construct(graph));

  return encpool;
}

Ptr<IModel> createBaseModelByType(std::string type, usage use, Ptr<Options> options) {
  Ptr<ExpressionGraph> graph = nullptr; // graph unknown at this stage
  // clang-format off

  if(type == "comet-qe" || type == "comet") {
    if(type == "comet") {
      ABORT_IF(use == usage::training, "Usage {} is not supported for model of type {}", (int)use, type); 
      ABORT_IF(use == usage::scoring, "Usage {} is not supported for model of type {}", (int)use, type); 
    }
    
    auto inputTypes = options->get<std::vector<std::string>>("input-types");
    ABORT_IF(inputTypes.empty(),
      "Required option --input-types for COMET-QE not set. "
      "For inference that should be --input-types sequence sequence. "
      "For training set --input-types class sequence sequence");

    int shift = 0;
    if(inputTypes[0] == "class")
      shift = 1;
    
    auto newOptions = options->with("usage", use);
    auto res = New<EncoderPooler>(newOptions);

    size_t numEncoders = 0;
    bool addMetricPooler = false;
    bool addEmbeddingPooler = false;

    switch(use) {
      case usage::embedding:  numEncoders = 1; addEmbeddingPooler = true; break;
      case usage::raw:
      case usage::evaluating:   
      case usage::scoring:
      case usage::training:   numEncoders = (type == "comet-qe") ? 2 : 3; addMetricPooler = true; break;
      default: ABORT("Usage {} is not supported for model of type {}", (int)use, type); 
    }
  
    for(size_t i = 0; i < numEncoders; i++) {
      auto enc = New<CometBatchEncoder>(graph, newOptions->with("type", "transformer", "index", i + shift));
      enc->setName("CometEncoder"); // parameters will be shared
      res->push_back(enc);
    }
    
    if(addEmbeddingPooler) {
      auto pooler = New<CometEmbeddingPooler>(graph, newOptions);
      pooler->setName("CometEmbeddingPooler"); 
      res->push_back(pooler);
    }
    
    if(addMetricPooler) {
      auto pooler = New<CometMetricPooler>(graph, newOptions);
      pooler->setName("CometQEPooler"); // @TODO: change name for different models
      res->push_back(pooler);
    }

    return res;
  }

  if(type == "bleurt") {
    ABORT_IF(use != usage::evaluating, "Usage other than 'evaluating' is not supported for model of type {}", type); 
    
    auto newOptions = options->with("usage", use);
    auto res = New<EncoderPooler>(newOptions);

    auto inputTypes = options->get<std::vector<std::string>>("input-types");
    ABORT_IF(inputTypes.empty(),
      "Required option --input-types for BLEURT not set. "
      "For inference that should be --input-types sequence. "
      "For training set --input-types class sequence");

    int shift = 0;
    if(inputTypes[0] == "class")
      shift = 1;
    
    auto enc = New<BleurtBatchEncoder>(graph, newOptions->with("type", "transformer", "index", 0 + shift));
    enc->setName("BleurtEncoder");
    res->push_back(enc);
          
    auto pooler = New<BleurtPooler>(graph, newOptions);
    pooler->setName("BleurtPooler");
    res->push_back(pooler);
    return res;
  }

  bool trainEmbedderRank = options->hasAndNotEmpty("train-embedder-rank");
  if(use == usage::embedding || trainEmbedderRank) { // hijacking an EncoderDecoder model for embedding only
    auto dimVocabs = options->get<std::vector<int>>("dim-vocabs");
    size_t fields = trainEmbedderRank ? dimVocabs.size() : 0;
    int dimVocab = dimVocabs[0];

    Ptr<Options> newOptions;
    if(options->get<bool>("compute-similarity", false)) {
      newOptions = options->with("usage", use,
                                 "original-type", type,
                                 "input-types", std::vector<std::string>({"sequence", "sequence"}),
                                 "dim-vocabs", std::vector<int>(2, dimVocab));
    } else if(trainEmbedderRank) {
       newOptions = options->with("usage", use,
                                  "original-type", type,
                                  "input-types", std::vector<std::string>(fields, "sequence"),
                                  "dim-vocabs", std::vector<int>(fields, dimVocab));
    } else {
      newOptions = options->with("usage", use,
                                 "original-type", type,
                                 "input-types", std::vector<std::string>({"sequence"}),
                                 "dim-vocabs", std::vector<int>(1, dimVocab));
    }
    
    auto res = New<EncoderPooler>(newOptions);      
    if(options->get<bool>("compute-similarity", false)) {
      res->push_back(models::encoder(newOptions->with("index", 0)).construct(graph));
      res->push_back(models::encoder(newOptions->with("index", 1)).construct(graph));
      res->push_back(New<SimPooler>(graph, newOptions->with("type", "sim-pooler")));
    } else if(trainEmbedderRank) {
      LOG(info, "Using {} input fields for embedder ranking training", fields);
      for(int i = 0; i < fields; ++i)
        res->push_back(models::encoder(newOptions->with("index", i)).construct(graph));
      res->push_back(New<SimPooler>(graph, newOptions->with("type", "sim-pooler")));
    } else {
      res->push_back(models::encoder(newOptions->with("index", 0)).construct(graph));
      if(type == "laser")
        res->push_back(New<MaxPooler>(graph, newOptions->with("type", "max-pooler")));
      else
        res->push_back(New<SlicePooler>(graph, newOptions->with("type", "slice-pooler")));
    }

    return res;
  }

  if(type == "s2s" || type == "amun" || type == "nematus") {
    return models::encoder_decoder(options->with(
         "usage", use,
         "original-type", type))
        .push_back(models::encoder()("type", "s2s"))
        .push_back(models::decoder()("type", "s2s"))
        .construct(graph);
  }

  else if(type == "transformer-new") {
    auto newOptions = options->with("usage", use);
    auto res = New<EncoderDecoder>(graph, newOptions);
    
    auto enc = New<TransformerBatchEncoder>(graph, newOptions->with("type", "transformer"));
    enc->setName("TransformerBatchEncoder");
    res->push_back(enc);
    
    auto dec = New<TransformerBatchDecoder>(graph, newOptions->with("type", "transformer"));
    dec->setName("TransformerBatchDecoder");
    res->push_back(dec);
    
    return res;
  }

  else if(type == "transformer") {
    const char* tflavor = std::getenv("TRANSFORMER_FLAVOR");
    if(tflavor && std::strcmp(tflavor, "experimental") == 0) {
      auto newOptions = options->with("usage", use);
      auto res = New<TransformerLegacy>(graph, newOptions);
      
      auto enc = New<TransformerBatchEncoder>(graph, newOptions->with("type", "transformer"));
      enc->setName("TransformerBatchEncoder");
      res->push_back(enc);
      
      auto dec = New<TransformerBatchDecoder>(graph, newOptions->with("type", "transformer"));
      dec->setName("TransformerBatchDecoder");
      res->push_back(dec);
      
      return res;
    } else {
      auto newOptions = options->with("usage", use);
      auto res = New<EncoderDecoder>(graph, newOptions);
      res->push_back(New<EncoderTransformer>(graph, newOptions->with("type", "transformer")));
      res->push_back(New<DecoderTransformer>(graph, newOptions->with("type", "transformer")));
      return res;
    }
  }

  else if(type == "transformer_s2s") {
    return models::encoder_decoder()(options)
        ("usage", use)
        ("original-type", type)
        .push_back(models::encoder()("type", "transformer"))
        .push_back(models::decoder()("type", "s2s"))
        .construct(graph);
  }

  else if(type == "lm") {
    auto idx = options->has("index") ? options->get<size_t>("index") : 0;
    std::vector<int> dimVocabs = options->get<std::vector<int>>("dim-vocabs");
    int vocab = dimVocabs[0];
    dimVocabs.resize(idx + 1);
    std::fill(dimVocabs.begin(), dimVocabs.end(), vocab);

    return models::encoder_decoder(options->with(
         "usage", use,
         "type", "s2s",
         "original-type", type))
        .push_back(models::decoder()
                   ("index", idx)
                   ("dim-vocabs", dimVocabs))
        .construct(graph);
  }

  else if(type == "multi-s2s") {
    size_t numEncoders = 2;
    auto ms2sFactory = models::encoder_decoder()(options)
        ("usage", use)
        ("type", "s2s")
        ("original-type", type);

    for(size_t i = 0; i < numEncoders; ++i) {
      auto prefix = "encoder" + std::to_string(i + 1);
      ms2sFactory.push_back(models::encoder()("prefix", prefix)("index", i));
    }

    ms2sFactory.push_back(models::decoder()("index", numEncoders));

    return ms2sFactory.construct(graph);
  }

  else if(type == "shared-multi-s2s") {
    size_t numEncoders = 2;
    auto ms2sFactory = models::encoder_decoder()(options)
        ("usage", use)
        ("type", "s2s")
        ("original-type", type);

    for(size_t i = 0; i < numEncoders; ++i) {
      auto prefix = "encoder";
      ms2sFactory.push_back(models::encoder()("prefix", prefix)("index", i));
    }

    ms2sFactory.push_back(models::decoder()("index", numEncoders));

    return ms2sFactory.construct(graph);
  }

  else if(type == "multi-transformer") {
    size_t numEncoders = 2;
    auto mtransFactory = models::encoder_decoder()(options)
        ("usage", use)
        ("type", "transformer")
        ("original-type", type);

    for(size_t i = 0; i < numEncoders; ++i) {
      auto prefix = "encoder" + std::to_string(i + 1);
      mtransFactory.push_back(models::encoder()("prefix", prefix)("index", i));
    }
    mtransFactory.push_back(models::decoder()("index", numEncoders));

    return mtransFactory.construct(graph);
  }

  else if(type == "shared-multi-transformer") {
    size_t numEncoders = 2;
    auto mtransFactory = models::encoder_decoder()(options)
        ("usage", use)
        ("type", "transformer")
        ("original-type", type);

    for(size_t i = 0; i < numEncoders; ++i) {
      auto prefix = "encoder";
      mtransFactory.push_back(models::encoder()("prefix", prefix)("index", i));
    }
    mtransFactory.push_back(models::decoder()("index", numEncoders));

    return mtransFactory.construct(graph);
  }

  else if(type == "lm-transformer") {
    auto idx = options->has("index") ? options->get<size_t>("index") : 0;
    std::vector<int> dimVocabs = options->get<std::vector<int>>("dim-vocabs");
    int vocab = dimVocabs[0];
    dimVocabs.resize(idx + 1);
    std::fill(dimVocabs.begin(), dimVocabs.end(), vocab);

    return models::encoder_decoder()(options)
        ("usage", use)
        ("type", "transformer")
        ("original-type", type)
        .push_back(models::decoder()
                   ("index", idx)
                   ("dim-vocabs", dimVocabs))
        .construct(graph);
  }

  else if(type == "bert") {                      // for full BERT training
    return models::encoder_classifier()(options) //
        ("original-type", "bert")                // so we can query this
        ("usage", use)                           //
        .push_back(models::encoder()             //
                   ("type", "bert-encoder")      // close to original transformer encoder
                   ("index", 0))                 //
        .push_back(models::classifier()          //
                   ("prefix", "masked-lm")       // prefix for parameter names
                   ("type", "bert-masked-lm")    //
                   ("index", 0))                 // multi-task learning with MaskedLM
        .push_back(models::classifier()          //
                   ("prefix", "next-sentence")   // prefix for parameter names
                   ("type", "bert-classifier")   //
                   ("index", 1))                 // next sentence prediction
        .construct(graph);
  }

  else if(type == "bert-classifier") {           // for BERT fine-tuning on non-BERT classification task
    return models::encoder_classifier()(options) //
        ("original-type", "bert-classifier")     // so we can query this if needed
        ("usage", use)                           //
        .push_back(models::encoder()             //
                   ("type", "bert-encoder")      //
                   ("index", 0))                 // close to original transformer encoder
        .push_back(models::classifier()          //
                   ("type", "bert-classifier")   //
                   ("index", 1))                 // next sentence prediction
        .construct(graph);
  }

#ifdef COMPILE_EXAMPLES
  else if(type == "mnist-ffnn")
    return New<MnistFeedForwardNet>(options);
#endif
#ifdef CUDNN
#ifdef COMPILE_EXAMPLES
  else if(type == "mnist-lenet")
    return New<MnistLeNet>(options);
#endif
  else if(type == "char-s2s") {
    return models::encoder_decoder()(options)
        ("usage", use)
        ("original-type", type)
        .push_back(models::encoder()("type", "char-s2s"))
        .push_back(models::decoder()("type", "s2s"))
        .construct(graph);
  }
#endif

  // clang-format on
  else
    ABORT("Unknown model type: {}", type);
}

Ptr<IModel> createModelFromOptions(Ptr<Options> options, usage use) {
  std::string type = options->get<std::string>("type");
  auto baseModel = createBaseModelByType(type, use, options);

  // add (log)softmax if requested
  if (use == usage::translation) {
    if(std::dynamic_pointer_cast<EncoderDecoder>(baseModel)) {
      return New<Stepwise>(std::dynamic_pointer_cast<EncoderDecoder>(baseModel), New<LogSoftmaxStep>());
    }
#ifdef COMPILE_EXAMPLES
    // note: 'usage::translation' here means 'inference'
    else if (std::dynamic_pointer_cast<MnistFeedForwardNet>(baseModel))
      return New<Scorer>(baseModel, New<MNISTLogsoftmax>());
#ifdef CUDNN
    else if (std::dynamic_pointer_cast<MnistLeNet>(baseModel))
      return New<Scorer>(baseModel, New<MNISTLogsoftmax>());
#endif
#endif
    else
      ABORT("'usage' parameter 'translation' cannot be applied to model type: {}", type);
  }
  else if (use == usage::raw || use == usage::embedding || use == usage::evaluating)
    return baseModel;
  else
    ABORT("'Usage' parameter must be 'translation' or 'raw'"); // I am actually not sure what this is supposed to mean any more.
}

Ptr<ICriterionFunction> createCriterionFunctionFromOptions(Ptr<Options> options, usage use) {
  std::string type = options->get<std::string>("type");
  auto baseModel = createBaseModelByType(type, use, options);

  // add cost function
  ABORT_IF(use != usage::training && use != usage::scoring, "'Usage' parameter must be 'training' or 'scoring'");
  // note: usage::scoring means "score the loss function", hence it uses a Trainer (not Scorer, which is for decoding)
  // @TODO: Should we define a new class that does not compute gradients?
  if (std::dynamic_pointer_cast<EncoderDecoder>(baseModel))
    return New<Trainer>(baseModel, New<EncoderDecoderCECost>(options));
  else if (std::dynamic_pointer_cast<EncoderClassifier>(baseModel))
    return New<Trainer>(baseModel, New<EncoderClassifierCECost>(options));
#ifdef COMPILE_EXAMPLES
  // @TODO: examples should be compiled optionally
  else if (std::dynamic_pointer_cast<MnistFeedForwardNet>(baseModel))
    return New<Trainer>(baseModel, New<MNISTCrossEntropyCost>());
#ifdef CUDNN
  else if (std::dynamic_pointer_cast<MnistLeNet>(baseModel))
    return New<Trainer>(baseModel, New<MNISTCrossEntropyCost>());
#endif
#endif
  else if (type == "comet-qe" && std::dynamic_pointer_cast<EncoderPooler>(baseModel))
    return New<Trainer>(baseModel, New<CometLoss>(options));
  else if (std::dynamic_pointer_cast<EncoderPooler>(baseModel))
    return New<Trainer>(baseModel, New<EncoderPoolerRankCost>(options));
  else
    ABORT("Criterion function unknown for model type: {}", type);
}

}  // namespace models
}  // namespace marian
